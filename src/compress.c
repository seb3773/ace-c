#include "ace/compress.h"

#include <stdlib.h>
#include <string.h>

#define ACE_TOK_BLOCK 16384u
#define ACE_HASH_BITS 15
#define ACE_HASH_SIZE (1u << ACE_HASH_BITS)

typedef struct {
    uint16_t main;
    uint16_t len_sym;
    uint32_t stored_dist;
    uint8_t mode;
    uint8_t mode_a;
    uint32_t mode_b;
} ace_tok_t;

static unsigned dist_bits(uint32_t stored)
{
    return (unsigned)ace_bit_length(stored);
}

static unsigned min_len_for_dist(uint32_t stored)
{
    if (stored <= ACE_LZ77_MAXDISTATLEN2)
        return 2;
    if (stored <= ACE_LZ77_MAXDISTATLEN3)
        return 3;
    return 4;
}

static int toks_push(ace_tok_t **toks, size_t *n, size_t *cap, ace_tok_t t)
{
    if (*n >= *cap) {
        size_t nc = *cap ? *cap * 2 : 256;
        ace_tok_t *p = (ace_tok_t *)realloc(*toks, nc * sizeof(*p));
        if (!p)
            return ACE_ERR_NOMEM;
        *toks = p;
        *cap = nc;
    }
    (*toks)[(*n)++] = t;
    return ACE_OK;
}

static int emit_literal(ace_tok_t **toks, size_t *n, size_t *cap, uint8_t b)
{
    ace_tok_t t;
    memset(&t, 0, sizeof(t));
    t.main = b;
    t.len_sym = 0;
    t.stored_dist = 0;
    return toks_push(toks, n, cap, t);
}

static int emit_match(ace_tok_t **toks, size_t *n, size_t *cap,
                      uint32_t hist[4], uint32_t stored, unsigned *length)
{
    ace_tok_t t;
    unsigned i;
    unsigned minl;
    unsigned len = *length;

    memset(&t, 0, sizeof(t));
    for (unsigned offset = 0; offset < 4; offset++) {
        i = 3 - offset;
        if (hist[i] == stored) {
            unsigned sub = (offset > 1) ? 3 : 2;
            if (len < sub)
                continue;
            if (len - sub > ACE_LZ77_NUMLENCODES - 1)
                len = sub + ACE_LZ77_NUMLENCODES - 1;
            t.main = (uint16_t)(256 + offset);
            t.len_sym = (uint16_t)(len - sub);
            {
                uint32_t tmp = hist[i];
                memmove(&hist[i], &hist[i + 1], (3 - i) * sizeof(uint32_t));
                hist[3] = tmp;
            }
            *length = len;
            return toks_push(toks, n, cap, t);
        }
    }

    minl = min_len_for_dist(stored);
    if (len < minl)
        return ACE_ERR_PARAM;
    if (len - minl > ACE_LZ77_NUMLENCODES - 1)
        len = minl + ACE_LZ77_NUMLENCODES - 1;
    t.main = (uint16_t)(260 + dist_bits(stored));
    t.len_sym = (uint16_t)(len - minl);
    t.stored_dist = stored;
    memmove(&hist[0], &hist[1], 3 * sizeof(uint32_t));
    hist[3] = stored;
    *length = len;
    return toks_push(toks, n, cap, t);
}

static unsigned match_len(const uint8_t *a, const uint8_t *b, unsigned maxl)
{
    unsigned n = 0;
    while (n < maxl && a[n] == b[n])
        n++;
    return n;
}

static unsigned quality_chain(unsigned quality)
{
    static const unsigned chain[] = {1, 4, 16, 64, 256, 1024};
    if (quality > 5)
        quality = 5;
    return chain[quality];
}

static unsigned quality_nice(unsigned quality)
{
    static const unsigned nice[] = {8, 16, 32, 64, 128, 258};
    if (quality > 5)
        quality = 5;
    return nice[quality];
}

static unsigned quality_lazy(unsigned quality)
{
    static const unsigned lazy[] = {0, 0, 4, 16, 32, 64};
    if (quality > 5)
        quality = 5;
    return lazy[quality];
}

static unsigned hash3(const uint8_t *p)
{
    return (((unsigned)p[0] << 8) ^ ((unsigned)p[1] << 4) ^ p[2]) & (ACE_HASH_SIZE - 1);
}

static void insert_hash(uint32_t *head, uint32_t *prev, size_t i, const uint8_t *in, size_t n)
{
    unsigned h;
    if (i + 2 >= n)
        return;
    h = hash3(in + i);
    prev[i + 1] = head[h];
    head[h] = (uint32_t)(i + 1);
}

static unsigned find_match(const uint8_t *in, size_t n, size_t i, size_t dicsize,
                           const uint32_t *prev, const uint32_t *head,
                           const uint32_t *head2, const uint32_t hist[4],
                           unsigned max_chain, unsigned nice,
                           uint32_t *out_dist)
{
    unsigned best_len = 0;
    uint32_t best_dist = 0;
    size_t rem = n - i;
    unsigned max_match;

    *out_dist = 0;
    if (rem <= 259)
        max_match = (rem > 1) ? (unsigned)(rem - 1) : 0;
    else
        max_match = 259;
    if (max_match > ACE_LZ77_NUMLENCODES + 3)
        max_match = ACE_LZ77_NUMLENCODES + 3;
    if (max_match < 2)
        return 0;

    /* 1. Repeat distances from hist */
    if (hist) {
        for (int offset = 0; offset < 4; offset++) {
            uint32_t stored = hist[3 - offset];
            uint32_t dist = stored + 1;
            unsigned minl = (offset > 1) ? 3 : 2;
            if (dist <= i && dist <= dicsize && max_match >= minl) {
                unsigned ml = max_match;
                unsigned lencap = minl + ACE_LZ77_NUMLENCODES - 1;
                if (ml > lencap)
                    ml = lencap;
                unsigned got = match_len(in + i - dist, in + i, ml);
                if (got >= minl && got > best_len) {
                    best_len = got;
                    best_dist = dist;
                    if (got >= nice)
                        break;
                }
            }
        }
        if (best_len >= nice) {
            *out_dist = best_dist;
            return best_len;
        }
    }

    /* 2. 2-byte hash table */
    if (head2 && i + 1 < n && max_match >= 2) {
        uint16_t h2 = (uint16_t)in[i] | ((uint16_t)in[i + 1] << 8);
        uint32_t p2 = head2[h2];
        if (p2 > 0) {
            uint32_t pos2 = p2 - 1;
            if (pos2 < i) {
                uint32_t dist = (uint32_t)(i - pos2);
                if (dist <= 256 && dist <= dicsize) {
                    unsigned ml = max_match;
                    unsigned lencap = 2 + ACE_LZ77_NUMLENCODES - 1;
                    if (ml > lencap)
                        ml = lencap;
                    unsigned got = match_len(in + pos2, in + i, ml);
                    if (got >= 2 && got > best_len) {
                        best_len = got;
                        best_dist = dist;
                    }
                }
            }
        }
        if (best_len >= nice) {
            *out_dist = best_dist;
            return best_len;
        }
    }

    /* 3. 3-byte hash table chain */
    if (i + 2 < n && max_match >= 3) {
        uint32_t p = head[hash3(in + i)];
        unsigned chain = 0;
        while (p && chain++ < max_chain) {
            uint32_t pos = p - 1;
            uint32_t dist;
            unsigned ml, got, minl, lencap;
            if (pos >= i) {
                p = prev[p];
                continue;
            }
            dist = (uint32_t)(i - pos);
            if (dist > dicsize || dist == 0)
                break;
            ml = max_match;
            minl = min_len_for_dist(dist - 1);
            if (minl > ml) {
                p = prev[p];
                continue;
            }
            lencap = minl + ACE_LZ77_NUMLENCODES - 1;
            if (ml > lencap)
                ml = lencap;
            got = match_len(in + pos, in + i, ml);
            if (got > lencap)
                got = lencap;
            if (got >= minl && got > best_len) {
                best_len = got;
                best_dist = dist;
                if (got >= nice)
                    break;
            }
            p = prev[p];
        }
    }

    *out_dist = best_dist;
    return best_len;
}

int ace_comp_hist_init(ace_comp_hist_t *h)
{
    if (!h)
        return ACE_ERR_PARAM;
    memset(h, 0, sizeof(*h));
    return ACE_OK;
}

void ace_comp_hist_free(ace_comp_hist_t *h)
{
    if (!h)
        return;
    free(h->data);
    memset(h, 0, sizeof(*h));
}

int ace_comp_hist_add(ace_comp_hist_t *h, const uint8_t *p, size_t n, size_t dicsize)
{
    uint8_t *nd;
    size_t keep;

    if (!h)
        return ACE_ERR_PARAM;
    if (!p || n == 0)
        return ACE_OK;
    if (dicsize < ACE_LZ77_MINDICSIZE)
        dicsize = ACE_LZ77_MINDICSIZE;
    if (h->n + n > h->cap) {
        size_t nc = h->cap ? h->cap : 4096;
        while (nc < h->n + n)
            nc *= 2;
        nd = (uint8_t *)realloc(h->data, nc);
        if (!nd)
            return ACE_ERR_NOMEM;
        h->data = nd;
        h->cap = nc;
    }
    memcpy(h->data + h->n, p, n);
    h->n += n;
    keep = dicsize;
    if (h->n > keep) {
        memmove(h->data, h->data + (h->n - keep), keep);
        h->n = keep;
    }
    return ACE_OK;
}

static int tokenize(const uint8_t *prefix, size_t prefix_n,
                    const uint8_t *in, size_t n, size_t dicsize, unsigned quality,
                    ace_tok_t **toks, size_t *ntoks)
{
    uint32_t *head;
    uint32_t *head2;
    uint32_t *prev;
    uint32_t hist[4];
    uint8_t *all = NULL;
    const uint8_t *src;
    size_t total, start, i, k;
    size_t cap = 0;
    int rc = ACE_OK;
    unsigned max_chain = quality_chain(quality);
    unsigned nice = quality_nice(quality);
    unsigned lazy = quality_lazy(quality);

    *toks = NULL;
    *ntoks = 0;
    if (prefix && prefix_n > dicsize) {
        prefix += prefix_n - dicsize;
        prefix_n = dicsize;
    }
    if (!prefix)
        prefix_n = 0;
    total = prefix_n + n;
    start = prefix_n;
    if (prefix_n) {
        all = (uint8_t *)malloc(total ? total : 1);
        if (!all)
            return ACE_ERR_NOMEM;
        memcpy(all, prefix, prefix_n);
        if (n)
            memcpy(all + prefix_n, in, n);
        src = all;
    } else {
        src = in;
        total = n;
        start = 0;
    }

    memset(hist, 0, sizeof(hist));
    head = (uint32_t *)calloc(ACE_HASH_SIZE, sizeof(uint32_t));
    head2 = (uint32_t *)calloc(65536, sizeof(uint32_t));
    prev = (uint32_t *)calloc(total + 1, sizeof(uint32_t));
    if (!head || !head2 || !prev) {
        free(head);
        free(head2);
        free(prev);
        free(all);
        return ACE_ERR_NOMEM;
    }
    for (i = 0; i < start; i++) {
        insert_hash(head, prev, i, src, total);
        if (i + 1 < total)
            head2[(uint16_t)src[i] | ((uint16_t)src[i + 1] << 8)] = (uint32_t)(i + 1);
    }

    i = start;
    while (i < total) {
        unsigned best_len;
        uint32_t best_dist = 0;
        insert_hash(head, prev, i, src, total);
        best_len = find_match(src, total, i, dicsize, prev, head, head2, hist, max_chain, nice, &best_dist);
        if (i + 1 < total)
            head2[(uint16_t)src[i] | ((uint16_t)src[i + 1] << 8)] = (uint32_t)(i + 1);

        if (best_len >= 2 && lazy > 0 && i + 1 < total && best_len < nice) {
            unsigned next_len;
            uint32_t next_dist = 0;
            next_len = find_match(src, total, i + 1, dicsize, prev, head, head2, hist,
                                  max_chain > lazy ? lazy : max_chain, nice, &next_dist);
            if (next_len > best_len + 1) {
                rc = emit_literal(toks, ntoks, &cap, src[i]);
                if (rc != ACE_OK)
                    break;
                i++;
                continue;
            }
        }
        if (best_len >= 2) {
            rc = emit_match(toks, ntoks, &cap, hist, best_dist - 1, &best_len);
            if (rc != ACE_OK)
                break;
            for (k = 1; k < best_len; k++) {
                insert_hash(head, prev, i + k, src, total);
                if (i + k + 1 < total)
                    head2[(uint16_t)src[i + k] | ((uint16_t)src[i + k + 1] << 8)] = (uint32_t)(i + k + 1);
            }
            i += best_len;
        } else {
            rc = emit_literal(toks, ntoks, &cap, src[i]);
            if (rc != ACE_OK)
                break;
            i++;
        }
    }
    free(head);
    free(head2);
    free(prev);
    free(all);
    return rc;
}

static int write_block(ace_bsw_t *w, const ace_tok_t *toks, size_t n)
{
    uint32_t main_freq[ACE_LZ77_NUMMAINCODES];
    uint32_t len_freq[ACE_LZ77_NUMLENCODES];
    ace_huff_t main_tree;
    ace_huff_t len_tree;
    size_t i;
    int rc;
    memset(main_freq, 0, sizeof(main_freq));
    memset(len_freq, 0, sizeof(len_freq));
    memset(&main_tree, 0, sizeof(main_tree));
    memset(&len_tree, 0, sizeof(len_tree));

    for (i = 0; i < n; i++) {
        main_freq[toks[i].main]++;
        if (toks[i].main >= 256 && toks[i].main < ACE_LZ77_TYPECODE)
            len_freq[toks[i].len_sym]++;
    }
    {
        unsigned distinct_len = 0;
        for (i = 0; i < ACE_LZ77_NUMLENCODES; i++) {
            if (len_freq[i])
                distinct_len++;
        }
        if (distinct_len < 2) {
            if (len_freq[0] == 0)
                len_freq[0] = 1;
            else
                len_freq[1] = 1;
        }
    }

    rc = ace_huff_from_freq(main_freq, ACE_LZ77_NUMMAINCODES, ACE_LZ77_MAXCODEWIDTH, &main_tree);
    if (rc == ACE_OK)
        rc = ace_huff_from_freq(len_freq, ACE_LZ77_NUMLENCODES, ACE_LZ77_MAXCODEWIDTH, &len_tree);
    if (rc == ACE_OK)
        rc = ace_huff_write_tree(w, &main_tree);
    if (rc == ACE_OK)
        rc = ace_huff_write_tree(w, &len_tree);
    if (rc == ACE_OK)
        rc = ace_bsw_write_bits(w, (uint32_t)n, 15);
    for (i = 0; rc == ACE_OK && i < n; i++) {
        const ace_tok_t *t = &toks[i];
        rc = ace_huff_write_symbol(w, &main_tree, t->main);
        if (rc != ACE_OK)
            break;
        if (t->main <= 255)
            continue;
        if (t->main <= 259) {
            rc = ace_huff_write_symbol(w, &len_tree, t->len_sym);
        } else if (t->main < ACE_LZ77_TYPECODE) {
            unsigned bits = (unsigned)(t->main - 260);
            rc = ace_bsw_write_knownwidth_uint(w, bits, t->stored_dist);
            if (rc == ACE_OK)
                rc = ace_huff_write_symbol(w, &len_tree, t->len_sym);
        } else if (t->main == ACE_LZ77_TYPECODE) {
            rc = ace_mode_write(w, t->mode, t->mode_a, t->mode_b);
        }
    }
    ace_huff_free(&main_tree);
    ace_huff_free(&len_tree);
    return rc;
}

static int write_tokens(ace_bsw_t *w, const ace_tok_t *toks, size_t ntoks)
{
    size_t off = 0;
    int rc = ACE_OK;
    if (ntoks == 0) {
        ace_tok_t dummy;
        memset(&dummy, 0, sizeof(dummy));
        return write_block(w, &dummy, 0);
    }
    while (rc == ACE_OK && off < ntoks) {
        size_t chunk = ntoks - off;
        if (chunk > ACE_TOK_BLOCK)
            chunk = ACE_TOK_BLOCK;
        rc = write_block(w, toks + off, chunk);
        off += chunk;
    }
    return rc;
}

static int write_mode_switch(ace_bsw_t *w, unsigned mode, unsigned a, unsigned b)
{
    ace_tok_t t;
    memset(&t, 0, sizeof(t));
    t.main = ACE_LZ77_TYPECODE;
    t.mode = (uint8_t)mode;
    t.mode_a = (uint8_t)a;
    t.mode_b = b;
    return write_block(w, &t, 1);
}

static void exe_preprocess(uint8_t *data, size_t n, uint64_t produced, unsigned exe_mode)
{
    size_t i = 0;
    while (i + 4 < n) {
        if (data[i] == 0xE8) {
            uint64_t pos = produced + i;
            if (exe_mode == 0) {
                uint16_t rel = (uint16_t)(data[i + 1] | (data[i + 2] << 8));
                rel = (uint16_t)(rel + (uint16_t)pos);
                data[i + 1] = (uint8_t)rel;
                data[i + 2] = (uint8_t)(rel >> 8);
                i += 3;
            } else {
                uint32_t rel = ace_r32(data + i + 1);
                rel = rel + (uint32_t)pos;
                data[i + 1] = (uint8_t)rel;
                data[i + 2] = (uint8_t)(rel >> 8);
                data[i + 3] = (uint8_t)(rel >> 16);
                data[i + 4] = (uint8_t)(rel >> 24);
                i += 5;
            }
        } else if (data[i] == 0xE9) {
            uint64_t pos = produced + i;
            uint16_t rel = (uint16_t)(data[i + 1] | (data[i + 2] << 8));
            rel = (uint16_t)(rel + (uint16_t)pos);
            data[i + 1] = (uint8_t)rel;
            data[i + 2] = (uint8_t)(rel >> 8);
            i += 3;
        } else {
            i++;
        }
    }
}

static void delta_preprocess(uint8_t *planar, const uint8_t *in, size_t n,
                             unsigned dist, int *last_delta)
{
    size_t plane_size = n / dist;
    size_t k, pos, i;
    for (k = 0; k < dist; k++) {
        for (pos = 0; pos < plane_size; pos++)
            planar[k * plane_size + pos] = in[pos * dist + k];
    }
    for (i = 0; i < n; i++) {
        int v = planar[i];
        planar[i] = (uint8_t)(v - *last_delta);
        *last_delta = v;
    }
}

static uint64_t absdiff_score(const uint8_t *p, size_t n, unsigned step)
{
    uint64_t s = 0;
    size_t i;
    if (n <= step)
        return 0;
    for (i = step; i < n; i++) {
        int d = (int)p[i] - (int)p[i - step];
        s += (uint64_t)(d < 0 ? -d : d);
    }
    return s;
}

static unsigned count_exe_ops(const uint8_t *in, size_t n)
{
    size_t i;
    unsigned c = 0;
    if (n < 5)
        return 0;
    for (i = 0; i + 4 < n; i++) {
        if (in[i] == 0xE8 || in[i] == 0xE9)
            c++;
    }
    return c;
}

static int detect_pic(const uint8_t *in, size_t n, int *width, int *planes)
{
    static const size_t cand[] = {
        16, 24, 32, 40, 48, 64, 80, 96, 128, 160, 176, 192,
        256, 320, 384, 512, 640, 720, 768, 800, 1024, 1280, 1600, 1920, 2048
    };
    size_t i, best_w = 0;
    uint64_t best = (uint64_t)-1;
    uint64_t s1;

    if (n < 512)
        return 0;
    s1 = absdiff_score(in, n, 1);
    if (s1 == 0)
        return 0;
    for (i = 0; i < sizeof(cand) / sizeof(cand[0]); i++) {
        size_t w = cand[i];
        uint64_t sh = 0;
        size_t rows, r, c;
        if (w * 2 > n || n % w)
            continue;
        rows = n / w;
        for (r = 0; r < rows; r++) {
            for (c = 1; c < w; c++) {
                int d = (int)in[r * w + c] - (int)in[r * w + c - 1];
                sh += (uint64_t)(d < 0 ? -d : d);
            }
        }
        if (sh < best) {
            best = sh;
            best_w = w;
        }
    }
    if (best_w && best * 2 < s1) {
        *width = (int)best_w;
        *planes = (best_w % 3 == 0 && best_w >= 24) ? 3 : 1;
        return 1;
    }
    return 0;
}

static unsigned detect_sound(const uint8_t *in, size_t n)
{
    uint64_t s1, s2, s4;
    if (n < 512 || (n & 3))
        return 0;
    s1 = absdiff_score(in, n, 1);
    s2 = absdiff_score(in, n, 2);
    s4 = absdiff_score(in, n, 4);

    if (s4 * 3 < s1 * 2 && s4 * 3 < s2 * 2)
        return ACE_MODE_SOUND_32A;
    if (s2 * 3 < s1 * 2 && s2 * 8 > s1)
        return ACE_MODE_SOUND_16;
    return 0;
}

static int encode_buf(ace_bsw_t *w, const uint8_t *prefix, size_t prefix_n,
                      const uint8_t *buf, size_t n, size_t dicsize,
                      unsigned quality, const ace_tok_t *switch_tok)
{
    ace_tok_t *toks = NULL;
    size_t ntoks = 0;
    int rc = tokenize(prefix, prefix_n, buf, n, dicsize, quality, &toks, &ntoks);
    if (rc != ACE_OK)
        return rc;
    if (switch_tok) {
        ace_tok_t *all_toks = (ace_tok_t *)malloc((ntoks + 1) * sizeof(ace_tok_t));
        if (!all_toks) {
            free(toks);
            return ACE_ERR_NOMEM;
        }
        all_toks[0] = *switch_tok;
        if (ntoks)
            memcpy(all_toks + 1, toks, ntoks * sizeof(ace_tok_t));
        free(toks);
        toks = all_toks;
        ntoks++;
    }
    rc = write_tokens(w, toks, ntoks);
    free(toks);
    return rc;
}

int ace_compress_lz77_hist(const uint8_t *in, size_t n, size_t dicsize,
                           unsigned quality, ace_comp_hist_t *hist,
                           uint8_t **out, size_t *out_n)
{
    ace_bsw_t w;
    int rc;
    const uint8_t *prefix = NULL;
    size_t prefix_n = 0;

    *out = NULL;
    *out_n = 0;
    if (dicsize < ACE_LZ77_MINDICSIZE)
        dicsize = ACE_LZ77_MINDICSIZE;
    if (dicsize > ACE_LZ77_MAXDICSIZE)
        dicsize = ACE_LZ77_MAXDICSIZE;
    if (quality > 5)
        quality = 5;
    if (hist && hist->n) {
        prefix = hist->data;
        prefix_n = hist->n;
    }

    rc = ace_bsw_init(&w);
    if (rc != ACE_OK)
        return rc;
    rc = encode_buf(&w, prefix, prefix_n, in, n, dicsize, quality, NULL);
    if (rc == ACE_OK)
        rc = ace_bsw_pad32(&w);
    if (rc != ACE_OK) {
        ace_bsw_free(&w);
        return rc;
    }
    *out = w.data;
    *out_n = w.len;
    w.data = NULL;
    ace_bsw_free(&w);
    if (hist)
        return ace_comp_hist_add(hist, in, n, dicsize);
    return ACE_OK;
}

int ace_compress_lz77(const uint8_t *in, size_t n, size_t dicsize,
                      unsigned quality, uint8_t **out, size_t *out_n)
{
    return ace_compress_lz77_hist(in, n, dicsize, quality, NULL, out, out_n);
}

#define ACE_RUN_CHUNK 32768u

typedef struct {
    unsigned mode;
    unsigned a;
    unsigned b;
    size_t off;
    size_t n;
    int pic_width;
    int pic_planes;
} ace_run_t;

static int is_text_data(const uint8_t *p, size_t n)
{
    size_t i, printable = 0;
    if (n == 0)
        return 0;
    for (i = 0; i < n; i++) {
        uint8_t b = p[i];
        if (b == '\r' || b == '\n' || b == '\t' || (b >= 32 && b <= 126))
            printable++;
    }
    return (printable * 100 / n) >= 90;
}

static int detect_delta(const uint8_t *p, size_t n, unsigned *delta_dist, unsigned *delta_len)
{
    uint64_t s1, s2, s4;
    size_t d, max_d;
    size_t best_d = 0;
    double best_pct = 0.0;
    size_t s1_matches = 0;
    size_t sample_n;

    if (n < 256 || is_text_data(p, n))
        return 0;

    sample_n = (n > 65536) ? 65536 : n;

    /* 1. Fast audio / 16-bit / 32-bit periodic delta checks */
    s1 = absdiff_score(p, sample_n, 1);
    s2 = absdiff_score(p, sample_n, 2);
    if (sample_n % 2 == 0 && s2 * 3 < s1 * 2) {
        *delta_dist = 2;
        *delta_len = (unsigned)sample_n;
        return 1;
    }
    s4 = absdiff_score(p, sample_n, 4);
    if (sample_n % 4 == 0 && s4 * 3 < s1 * 2) {
        *delta_dist = 4;
        *delta_len = (unsigned)sample_n;
        return 1;
    }

    /* 2. Check adjacent byte equality to ignore flat runs (e.g. constant byte fills) */
    for (size_t i = 1; i < sample_n; i++) {
        if (p[i] == p[i - 1])
            s1_matches++;
    }
    if ((double)s1_matches / (double)(sample_n - 1) >= 0.50)
        return 0;

    /* 3. Arbitrary periodicity search for 2D rasters, columnar / planar data */
    max_d = sample_n / 4;
    if (max_d > 128)
        max_d = 128;
    for (d = 2; d <= max_d; d++) {
        size_t i, matches = 0, total = sample_n - d;
        double pct;
        for (i = d; i < sample_n; i++) {
            if (p[i] == p[i - d])
                matches++;
        }
        pct = (double)matches / (double)total;
        if (pct >= 0.70 && pct > best_pct) {
            best_pct = pct;
            best_d = d;
        }
    }

    if (best_d) {
        /* Check if any sub-divisor of best_d also matches >= 70% to find fundamental period */
        for (d = 2; d < best_d; d++) {
            if (best_d % d == 0) {
                size_t i, matches = 0, total = sample_n - d;
                for (i = d; i < sample_n; i++) {
                    if (p[i] == p[i - d])
                        matches++;
                }
                if ((double)matches / (double)total >= 0.70) {
                    best_d = d;
                    break;
                }
            }
        }
        *delta_dist = (unsigned)best_d;
        *delta_len = (unsigned)((sample_n / best_d) * best_d);
        return 1;
    }

    return 0;
}

static unsigned pick_mode(const uint8_t *p, size_t n, unsigned *delta_dist, unsigned *delta_len,
                          int *pic_w, int *pic_p)
{
    unsigned sm;

    *delta_dist = 2;
    *delta_len = (unsigned)n;
    *pic_w = 0;
    *pic_p = 1;

    if (n >= 256) {
        unsigned ops = count_exe_ops(p, n);
        if (ops >= 4 && (ops * 32u >= (unsigned)n || (n >= 512 && p[0] == 'M' && p[1] == 'Z')))
            return ACE_MODE_LZ77_EXE;
    }

    sm = detect_sound(p, n);
    if (sm)
        return sm;

    if (detect_delta(p, n, delta_dist, delta_len))
        return ACE_MODE_LZ77_DELTA;

    if (detect_pic(p, n, pic_w, pic_p))
        return ACE_MODE_PIC;

    return ACE_MODE_LZ77;
}

static int runs_push(ace_run_t **runs, size_t *n, size_t *cap, ace_run_t r)
{
    if (*n >= *cap) {
        size_t nc = *cap ? *cap * 2 : 8;
        ace_run_t *p = (ace_run_t *)realloc(*runs, nc * sizeof(*p));
        if (!p)
            return ACE_ERR_NOMEM;
        *runs = p;
        *cap = nc;
    }
    (*runs)[(*n)++] = r;
    return ACE_OK;
}

static int build_runs(const uint8_t *in, size_t n, unsigned force_mode,
                      int pic_width, int pic_planes, ace_run_t **runs, size_t *nruns)
{
    size_t cap = 0, off = 0;
    int rc = ACE_OK;
    ace_run_t whole;
    unsigned delta_dist = 2, delta_len = (unsigned)n;
    int pw = 0, pp = 1;

    *runs = NULL;
    *nruns = 0;
    memset(&whole, 0, sizeof(whole));
    whole.off = 0;
    whole.n = n;
    whole.pic_width = pic_width;
    whole.pic_planes = pic_planes ? pic_planes : 1;

    if (force_mode) {
        whole.mode = force_mode;
        if (force_mode == ACE_MODE_LZ77_EXE) {
            whole.a = 1;
            return runs_push(runs, nruns, &cap, whole);
        } else if (force_mode == ACE_MODE_LZ77_DELTA) {
            delta_dist = 2;
        } else {
            return runs_push(runs, nruns, &cap, whole);
        }
    } else {
        whole.mode = pick_mode(in, n, &delta_dist, &delta_len, &pw, &pp);
    }

    if (whole.mode != ACE_MODE_LZ77 || n <= ACE_RUN_CHUNK) {
        if (whole.mode == ACE_MODE_LZ77_EXE) {
            whole.a = 1;
            return runs_push(runs, nruns, &cap, whole);
        } else if (whole.mode == ACE_MODE_LZ77_DELTA) {
            size_t off = 0;
            size_t max_chunk = (65536u / delta_dist) * delta_dist;
            if (max_chunk == 0)
                max_chunk = 65536u;
            while (off < n) {
                size_t rem = n - off;
                size_t chunk_len = (rem > max_chunk) ? max_chunk : (rem / delta_dist) * delta_dist;
                if (chunk_len == 0) {
                    ace_run_t trail;
                    memset(&trail, 0, sizeof(trail));
                    trail.mode = ACE_MODE_LZ77;
                    trail.off = off;
                    trail.n = rem;
                    return runs_push(runs, nruns, &cap, trail);
                }
                ace_run_t drun;
                memset(&drun, 0, sizeof(drun));
                drun.mode = ACE_MODE_LZ77_DELTA;
                drun.a = delta_dist;
                drun.b = (unsigned)chunk_len;
                drun.off = off;
                drun.n = chunk_len;
                rc = runs_push(runs, nruns, &cap, drun);
                if (rc != ACE_OK)
                    return rc;
                off += chunk_len;
            }
            return rc;
        } else if (whole.mode == ACE_MODE_PIC) {
            whole.pic_width = pw;
            whole.pic_planes = pp;
            return runs_push(runs, nruns, &cap, whole);
        } else {
            return runs_push(runs, nruns, &cap, whole);
        }
    }

    while (off < n) {
        ace_run_t r;
        size_t chunk = n - off;
        unsigned mode;
        if (chunk > ACE_RUN_CHUNK)
            chunk = ACE_RUN_CHUNK;
        if ((n - off) - chunk < 256 && chunk < n - off)
            chunk = n - off;
        memset(&r, 0, sizeof(r));
        mode = pick_mode(in + off, chunk, &delta_dist, &delta_len, &pw, &pp);
        r.mode = mode;
        r.off = off;
        r.n = chunk;
        r.pic_width = pw;
        r.pic_planes = pp;
        if (mode == ACE_MODE_LZ77_EXE)
            r.a = 1;
        else if (mode == ACE_MODE_LZ77_DELTA) {
            r.a = delta_dist;
            r.b = delta_len;
            r.n = delta_len;
        }
        if (*nruns && (*runs)[*nruns - 1].mode == r.mode &&
            r.mode != ACE_MODE_LZ77_DELTA && r.mode != ACE_MODE_PIC) {
            (*runs)[*nruns - 1].n += r.n;
        } else {
            rc = runs_push(runs, nruns, &cap, r);
            if (rc != ACE_OK)
                return rc;
            if (mode == ACE_MODE_LZ77_DELTA && delta_len < chunk) {
                ace_run_t trail;
                memset(&trail, 0, sizeof(trail));
                trail.mode = ACE_MODE_LZ77;
                trail.off = off + delta_len;
                trail.n = chunk - delta_len;
                rc = runs_push(runs, nruns, &cap, trail);
                if (rc != ACE_OK)
                    return rc;
            }
        }
        off += chunk;
    }
    return rc;
}

static int switch_from(ace_bsw_t *w, unsigned from, unsigned to, unsigned a, unsigned b,
                       ace_sound_t *snd)
{
    if (from == to && from == ACE_MODE_LZ77)
        return ACE_OK;
    if (from == ACE_MODE_LZ77 || from == ACE_MODE_LZ77_EXE || from == ACE_MODE_LZ77_DELTA)
        return write_mode_switch(w, to, a, b);
    if (from >= ACE_MODE_SOUND_8 && from <= ACE_MODE_SOUND_32B)
        return ace_sound_write_term(snd, w, to, a, b);
    if (from == ACE_MODE_PIC)
        return ace_pic_write_term(w, to, a, b);
    return write_mode_switch(w, to, a, b);
}

static int encode_run(ace_bsw_t *w, const ace_run_t *r, const uint8_t *in,
                      size_t dicsize, unsigned quality,
                      const uint8_t *prefix, size_t prefix_n,
                      ace_sound_t **snd, ace_pic_t **pic, int *last_delta,
                      uint64_t produced, const ace_tok_t *switch_tok)
{
    const uint8_t *p = in + r->off;
    size_t n = r->n;
    uint8_t *work = NULL;
    int rc = ACE_OK;

    if (r->mode == ACE_MODE_LZ77) {
        return encode_buf(w, prefix, prefix_n, p, n, dicsize, quality, switch_tok);
    }
    if (r->mode == ACE_MODE_LZ77_EXE) {
        work = (uint8_t *)malloc(n ? n : 1);
        if (!work)
            return ACE_ERR_NOMEM;
        memcpy(work, p, n);
        exe_preprocess(work, n, produced, r->a);
        rc = encode_buf(w, prefix, prefix_n, work, n, dicsize, quality, switch_tok);
        free(work);
        return rc;
    }
    if (r->mode == ACE_MODE_LZ77_DELTA) {
        work = (uint8_t *)malloc(n ? n : 1);
        if (!work)
            return ACE_ERR_NOMEM;
        delta_preprocess(work, p, n, r->a, last_delta);
        rc = encode_buf(w, prefix, prefix_n, work, n, dicsize, quality, switch_tok);
        free(work);
        return rc;
    }
    if (r->mode >= ACE_MODE_SOUND_8 && r->mode <= ACE_MODE_SOUND_32B) {
        size_t sound_n = n & ~(size_t)3;
        if (!*snd) {
            rc = ace_sound_init(snd);
            if (rc != ACE_OK)
                return rc;
        }
        rc = ace_sound_reinit(*snd, r->mode);
        if (rc == ACE_OK && sound_n)
            rc = ace_sound_write(*snd, w, p, sound_n);
        if (rc == ACE_OK && sound_n < n) {
            uint8_t *pre2 = NULL;
            size_t pn2 = prefix_n + sound_n;
            rc = ace_sound_write_term(*snd, w, ACE_MODE_LZ77, 0, 0);
            if (rc == ACE_OK && prefix_n) {
                pre2 = (uint8_t *)malloc(pn2);
                if (!pre2)
                    rc = ACE_ERR_NOMEM;
                else {
                    memcpy(pre2, prefix, prefix_n);
                    memcpy(pre2 + prefix_n, p, sound_n);
                }
            }
            if (rc == ACE_OK)
                rc = encode_buf(w, prefix_n ? pre2 : p, pn2,
                                p + sound_n, n - sound_n, dicsize, quality, NULL);
            free(pre2);
        }
        return rc;
    }
    if (r->mode == ACE_MODE_PIC) {
        int pw = r->pic_width;
        int pp = r->pic_planes;
        size_t used;
        if (pw <= 0)
            pw = (int)n;
        if (pp <= 0)
            pp = 1;
        used = ((size_t)pw > 0) ? (n / (size_t)pw) * (size_t)pw : 0;
        if (!*pic) {
            rc = ace_pic_init(pic);
            if (rc != ACE_OK)
                return rc;
        }
        rc = ace_pic_write_init(*pic, w, pw, pp);
        if (rc == ACE_OK)
            rc = ace_pic_write(*pic, w, p, used);
        if (rc == ACE_OK && used < n) {
            uint8_t *pre2 = NULL;
            size_t pn2 = prefix_n + used;
            rc = ace_pic_write_term(w, ACE_MODE_LZ77, 0, 0);
            if (rc == ACE_OK && prefix_n) {
                pre2 = (uint8_t *)malloc(pn2);
                if (!pre2)
                    rc = ACE_ERR_NOMEM;
                else {
                    memcpy(pre2, prefix, prefix_n);
                    memcpy(pre2 + prefix_n, p, used);
                }
            }
            if (rc == ACE_OK)
                rc = encode_buf(w, prefix_n ? pre2 : p, pn2,
                                p + used, n - used, dicsize, quality, NULL);
            free(pre2);
        }
        return rc;
    }
    return ACE_ERR_METHOD;
}

static int is_lz77_mode(unsigned mode)
{
    return mode == ACE_MODE_LZ77 || mode == ACE_MODE_LZ77_DELTA || mode == ACE_MODE_LZ77_EXE;
}

int ace_compress_blocked_hist(const uint8_t *in, size_t n, size_t dicsize,
                              unsigned quality, unsigned force_mode,
                              int pic_width, int pic_planes,
                              ace_comp_hist_t *hist,
                              uint8_t **out, size_t *out_n)
{
    ace_bsw_t w;
    ace_run_t *runs = NULL;
    size_t nruns = 0, i;
    int rc;
    unsigned cur = ACE_MODE_LZ77;
    ace_sound_t *snd = NULL;
    ace_pic_t *pic = NULL;
    int last_delta = 0;
    const uint8_t *prefix = NULL;
    size_t prefix_n = 0;
    uint8_t *full_prefix = NULL;
    size_t full_prefix_n = 0;
    ace_tok_t *lz_toks = NULL;
    size_t n_lz_toks = 0, cap_lz_toks = 0;
    /*
     * Mirror of what the decoder's LZ77 dictionary receives for this member:
     * raw bytes for LZ77/SOUND/PIC runs, but the *preprocessed* bytes for
     * EXE (E8/E9 translated) and DELTA (planar delta) runs.  Solid history
     * and intra-member prefixes must be built from this, not from raw input.
     */
    uint8_t *mirror = NULL;

    *out = NULL;
    *out_n = 0;
    if (dicsize < ACE_LZ77_MINDICSIZE)
        dicsize = ACE_LZ77_MINDICSIZE;
    if (dicsize > ACE_LZ77_MAXDICSIZE)
        dicsize = ACE_LZ77_MAXDICSIZE;
    if (quality > 5)
        quality = 5;
    if (hist && hist->n) {
        prefix = hist->data;
        prefix_n = hist->n;
    }

    rc = build_runs(in, n, force_mode, pic_width, pic_planes, &runs, &nruns);
    if (rc != ACE_OK) {
        free(runs);
        return rc;
    }
    rc = ace_bsw_init(&w);
    if (rc != ACE_OK) {
        free(runs);
        return rc;
    }

    if (prefix_n) {
        full_prefix = (uint8_t *)malloc(prefix_n + (n ? n : 1));
        if (!full_prefix) {
            ace_bsw_free(&w);
            free(runs);
            return ACE_ERR_NOMEM;
        }
        memcpy(full_prefix, prefix, prefix_n);
        full_prefix_n = prefix_n;
    }
    if (hist) {
        mirror = (uint8_t *)malloc(n ? n : 1);
        if (!mirror) {
            free(full_prefix);
            ace_bsw_free(&w);
            free(runs);
            return ACE_ERR_NOMEM;
        }
    }

    for (i = 0; rc == ACE_OK && i < nruns; i++) {
        ace_run_t *r = &runs[i];
        unsigned a = r->a, b = r->b;
        const uint8_t *pre;
        size_t pn;

        if (is_lz77_mode(r->mode)) {
            /* If transitioning mode or starting a new DELTA run, append TYPECODE 283 token */
            if (r->mode == ACE_MODE_LZ77_DELTA || cur != r->mode || (i == 0 && r->mode != ACE_MODE_LZ77)) {
                ace_tok_t sw_tok;
                memset(&sw_tok, 0, sizeof(sw_tok));
                sw_tok.main = ACE_LZ77_TYPECODE;
                sw_tok.mode = (uint8_t)r->mode;
                sw_tok.mode_a = (uint8_t)a;
                sw_tok.mode_b = b;
                rc = toks_push(&lz_toks, &n_lz_toks, &cap_lz_toks, sw_tok);
                if (rc != ACE_OK)
                    break;
            }

            /* DELTA runs or runs right after DELTA do not use pre-delta raw buffer as dictionary prefix */
            if (r->mode == ACE_MODE_LZ77_DELTA || cur == ACE_MODE_LZ77_DELTA) {
                pre = NULL;
                pn = 0;
            } else {
                pre = full_prefix ? full_prefix : prefix;
                pn = full_prefix ? full_prefix_n : prefix_n;
            }

            ace_tok_t *run_toks = NULL;
            size_t n_run_toks = 0;

            if (r->mode == ACE_MODE_LZ77) {
                rc = tokenize(pre, pn, in + r->off, r->n, dicsize, quality, &run_toks, &n_run_toks);
            } else if (r->mode == ACE_MODE_LZ77_DELTA) {
                uint8_t *work = (uint8_t *)malloc(r->n ? r->n : 1);
                if (!work)
                    rc = ACE_ERR_NOMEM;
                else {
                    delta_preprocess(work, in + r->off, r->n, r->a, &last_delta);
                    rc = tokenize(pre, pn, work, r->n, dicsize, quality, &run_toks, &n_run_toks);
                    if (mirror)
                        memcpy(mirror + r->off, work, r->n);
                    free(work);
                }
            } else if (r->mode == ACE_MODE_LZ77_EXE) {
                uint8_t *work = (uint8_t *)malloc(r->n ? r->n : 1);
                if (!work)
                    rc = ACE_ERR_NOMEM;
                else {
                    memcpy(work, in + r->off, r->n);
                    exe_preprocess(work, r->n, r->off, r->a);
                    rc = tokenize(pre, pn, work, r->n, dicsize, quality, &run_toks, &n_run_toks);
                    if (mirror)
                        memcpy(mirror + r->off, work, r->n);
                    free(work);
                }
            }

            if (rc == ACE_OK) {
                for (size_t k = 0; k < n_run_toks; k++) {
                    rc = toks_push(&lz_toks, &n_lz_toks, &cap_lz_toks, run_toks[k]);
                    if (rc != ACE_OK)
                        break;
                }
            }
            free(run_toks);

            /* Flush LZ77 tokens if this is the last run or next run is not LZ77 */
            if (rc == ACE_OK && (i + 1 == nruns || !is_lz77_mode(runs[i + 1].mode))) {
                if (n_lz_toks > 0) {
                    rc = write_tokens(&w, lz_toks, n_lz_toks);
                    n_lz_toks = 0;
                }
            }
        } else {
            /* Non-LZ77 mode (SOUND or PIC) */
            if (n_lz_toks > 0) {
                rc = write_tokens(&w, lz_toks, n_lz_toks);
                n_lz_toks = 0;
                if (rc != ACE_OK)
                    break;
            }
            if (cur != r->mode) {
                rc = switch_from(&w, cur, r->mode, a, b, snd);
                if (rc != ACE_OK)
                    break;
            }
            pre = full_prefix ? full_prefix : prefix;
            pn = full_prefix ? full_prefix_n : prefix_n;
            rc = encode_run(&w, r, in, dicsize, quality, pre, pn, &snd, &pic,
                            &last_delta, r->off, NULL);
        }

        if (rc == ACE_OK && mirror &&
            r->mode != ACE_MODE_LZ77_DELTA && r->mode != ACE_MODE_LZ77_EXE)
            memcpy(mirror + r->off, in + r->off, r->n);
        if (rc == ACE_OK && full_prefix) {
            memcpy(full_prefix + full_prefix_n, mirror ? mirror + r->off : in + r->off, r->n);
            full_prefix_n += r->n;
        }
        cur = r->mode;
        if (r->mode == ACE_MODE_PIC && r->n % (r->pic_width > 0 ? (size_t)r->pic_width : r->n))
            cur = ACE_MODE_LZ77;
        if (r->mode >= ACE_MODE_SOUND_8 && r->mode <= ACE_MODE_SOUND_32B && (r->n & 3))
            cur = ACE_MODE_LZ77;
    }

    free(lz_toks);
    ace_sound_free(snd);
    ace_pic_free(pic);
    free(full_prefix);
    free(runs);
    if (rc == ACE_OK)
        rc = ace_bsw_pad32(&w);
    if (rc != ACE_OK) {
        free(mirror);
        ace_bsw_free(&w);
        return rc;
    }
    *out = w.data;
    *out_n = w.len;
    w.data = NULL;
    ace_bsw_free(&w);
    if (hist) {
        rc = ace_comp_hist_add(hist, mirror, n, dicsize);
        free(mirror);
        return rc;
    }
    return ACE_OK;
}

int ace_compress_blocked_ex(const uint8_t *in, size_t n, size_t dicsize,
                            unsigned quality, unsigned force_mode,
                            int pic_width, int pic_planes,
                            uint8_t **out, size_t *out_n)
{
    return ace_compress_blocked_hist(in, n, dicsize, quality, force_mode,
                                     pic_width, pic_planes, NULL, out, out_n);
}

int ace_compress_blocked(const uint8_t *in, size_t n, size_t dicsize,
                         unsigned quality, uint8_t **out, size_t *out_n)
{
    return ace_compress_blocked_hist(in, n, dicsize, quality, 0, 0, 0, NULL, out, out_n);
}
