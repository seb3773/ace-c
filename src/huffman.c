#include "ace/huffman.h"

#include <stdlib.h>
#include <string.h>

void ace_huff_free(ace_huff_t *t)
{
    if (!t)
        return;
    free(t->codes);
    free(t->widths);
    free(t->enc_codes);
    t->codes = NULL;
    t->widths = NULL;
    t->enc_codes = NULL;
    t->ncodes = 0;
    t->nwidths = 0;
}

static void list_swap_u8(uint8_t *a, int i, int j)
{
    uint8_t t = a[i];
    a[i] = a[j];
    a[j] = t;
}

static void list_swap_u16(uint16_t *a, int i, int j)
{
    uint16_t t = a[i];
    a[i] = a[j];
    a[j] = t;
}

static void quicksort_subrange(uint8_t *keys, uint16_t *values, int left, int right)
{
    int new_left = left;
    int new_right = right;
    uint8_t m = keys[right];

    for (;;) {
        while (keys[new_left] > m)
            new_left++;
        while (keys[new_right] < m)
            new_right--;
        if (new_left <= new_right) {
            list_swap_u8(keys, new_left, new_right);
            list_swap_u16(values, new_left, new_right);
            new_left++;
            new_right--;
        }
        if (new_left >= new_right)
            break;
    }
    if (left < new_right) {
        if (left < new_right - 1)
            quicksort_subrange(keys, values, left, new_right);
        else if (keys[left] < keys[new_right]) {
            list_swap_u8(keys, left, new_right);
            list_swap_u16(values, left, new_right);
        }
    }
    if (right > new_left) {
        if (new_left < right - 1)
            quicksort_subrange(keys, values, new_left, right);
        else if (keys[new_left] < keys[right]) {
            list_swap_u8(keys, new_left, right);
            list_swap_u16(values, new_left, right);
        }
    }
}

static void ace_huff_quicksort(uint8_t *keys, uint16_t *values, int n)
{
    if (n > 1)
        quicksort_subrange(keys, values, 0, n - 1);
}

static int ace_huff_make_tree(const uint8_t *widths_in, unsigned nwidths,
                              unsigned max_width, ace_huff_t *out)
{
    uint8_t *sorted_widths;
    uint16_t *sorted_symbols;
    unsigned i;
    unsigned used;
    unsigned max_codes;
    unsigned pos;
    uint8_t *widths;

    memset(out, 0, sizeof(*out));
    if (nwidths == 0)
        return ACE_ERR_PARAM;
    widths = (uint8_t *)malloc(nwidths);
    sorted_widths = (uint8_t *)malloc(nwidths);
    sorted_symbols = (uint16_t *)malloc(nwidths * sizeof(uint16_t));
    if (!widths || !sorted_widths || !sorted_symbols) {
        free(widths);
        free(sorted_widths);
        free(sorted_symbols);
        return ACE_ERR_NOMEM;
    }
    memcpy(widths, widths_in, nwidths);
    memcpy(sorted_widths, widths_in, nwidths);
    for (i = 0; i < nwidths; i++)
        sorted_symbols[i] = (uint16_t)i;

    ace_huff_quicksort(sorted_widths, sorted_symbols, (int)nwidths);

    used = 0;
    while (used < nwidths && sorted_widths[used] != 0)
        used++;

    /*
     * Degenerate tree (0 or 1 used symbol): exactly like the reference
     * decoder, force the first sorted symbol to width 1 but keep its
     * original sorted width for code table construction.  No synthetic
     * second symbol is added, so the lone symbol occupies the low end of
     * the code table (bit 0), as emitted by WinACE.
     */
    if (used < 2) {
        widths[sorted_symbols[0]] = 1;
        if (used == 0)
            used = 1;
    }

    max_codes = 1u << max_width;
    out->codes = (uint16_t *)malloc(max_codes * sizeof(uint16_t));
    if (!out->codes) {
        free(widths);
        free(sorted_widths);
        free(sorted_symbols);
        return ACE_ERR_NOMEM;
    }
    pos = 0;
    for (i = used; i-- > 0;) {
        uint16_t sym = sorted_symbols[i];
        uint8_t wdt = sorted_widths[i];
        unsigned repeat;
        unsigned k;

        if (wdt > max_width) {
            free(widths);
            free(sorted_widths);
            free(sorted_symbols);
            ace_huff_free(out);
            return ACE_ERR_CORRUPT;
        }
        repeat = 1u << (max_width - wdt);
        if (pos + repeat > max_codes) {
            free(widths);
            free(sorted_widths);
            free(sorted_symbols);
            ace_huff_free(out);
            return ACE_ERR_CORRUPT;
        }
        for (k = 0; k < repeat; k++)
            out->codes[pos++] = sym;
    }
    out->ncodes = pos;
    out->enc_codes = (uint16_t *)calloc(nwidths, sizeof(uint16_t));
    if (!out->enc_codes) {
        free(widths);
        free(sorted_widths);
        free(sorted_symbols);
        ace_huff_free(out);
        return ACE_ERR_NOMEM;
    }
    pos = 0;
    for (i = used; i-- > 0;) {
        uint16_t sym = sorted_symbols[i];
        uint8_t wdt = sorted_widths[i];
        unsigned shift;
        if (wdt == 0 || wdt > max_width)
            continue;
        shift = max_width - wdt;
        out->enc_codes[sym] = (uint16_t)(pos >> shift);
        pos += 1u << shift;
    }
    out->widths = widths;
    out->nwidths = nwidths;
    out->max_width = max_width;
    free(sorted_widths);
    free(sorted_symbols);
    return ACE_OK;
}

int ace_huff_read_tree(ace_bs_t *bs, unsigned max_width, unsigned num_codes, ace_huff_t *out)
{
    uint32_t num_widths;
    uint32_t lower_width;
    uint32_t upper_width;
    uint8_t width_widths[16];
    ace_huff_t width_tree;
    uint8_t *widths;
    unsigned i;
    int rc;

    memset(out, 0, sizeof(*out));
    num_widths = ace_bs_read_bits(bs, 9);
    if (num_widths == ACE_BS_EOF)
        return ACE_ERR_EOF;
    num_widths += 1;
    if (num_widths > num_codes + 1)
        num_widths = num_codes + 1;
    lower_width = ace_bs_read_bits(bs, 4);
    upper_width = ace_bs_read_bits(bs, 4);
    if (lower_width == ACE_BS_EOF || upper_width == ACE_BS_EOF)
        return ACE_ERR_EOF;
    if (upper_width + 1 > 16)
        return ACE_ERR_CORRUPT;

    for (i = 0; i < upper_width + 1; i++) {
        uint32_t w = ace_bs_read_bits(bs, ACE_HUFF_WIDTHWIDTHBITS);
        if (w == ACE_BS_EOF)
            return ACE_ERR_EOF;
        width_widths[i] = (uint8_t)w;
    }
    rc = ace_huff_make_tree(width_widths, upper_width + 1, ACE_HUFF_MAXWIDTHWIDTH, &width_tree);
    if (rc != ACE_OK)
        return rc;

    widths = (uint8_t *)calloc(num_widths, 1);
    if (!widths) {
        ace_huff_free(&width_tree);
        return ACE_ERR_NOMEM;
    }
    i = 0;
    while (i < num_widths) {
        unsigned symbol;
        rc = ace_huff_read_symbol(&width_tree, bs, &symbol);
        if (rc != ACE_OK) {
            free(widths);
            ace_huff_free(&width_tree);
            return rc;
        }
        if (symbol < upper_width) {
            widths[i++] = (uint8_t)symbol;
        } else {
            uint32_t length = ace_bs_read_bits(bs, 4);
            unsigned k;
            if (length == ACE_BS_EOF) {
                free(widths);
                ace_huff_free(&width_tree);
                return ACE_ERR_EOF;
            }
            length += 4;
            if (length > num_widths - i)
                length = num_widths - i;
            for (k = 0; k < length; k++)
                widths[i++] = 0;
        }
    }
    ace_huff_free(&width_tree);

    if (upper_width > 0) {
        for (i = 1; i < num_widths; i++)
            widths[i] = (uint8_t)((widths[i] + widths[i - 1]) % upper_width);
    }
    for (i = 0; i < num_widths; i++) {
        if (widths[i] > 0)
            widths[i] = (uint8_t)(widths[i] + lower_width);
    }

    rc = ace_huff_make_tree(widths, num_widths, max_width, out);
    free(widths);
    return rc;
}

int ace_huff_read_symbol(const ace_huff_t *t, ace_bs_t *bs, unsigned *sym)
{
    uint32_t maxwidth_code;
    unsigned symbol;
    uint32_t rv;

    if (!t || !t->codes || t->max_width == 0)
        return ACE_ERR_CORRUPT;
    maxwidth_code = ace_bs_peek_bits(bs, t->max_width);
    if (maxwidth_code >= t->ncodes)
        return ACE_ERR_CORRUPT;
    symbol = t->codes[maxwidth_code];
    if (symbol >= t->nwidths)
        return ACE_ERR_CORRUPT;
    rv = ace_bs_skip_bits(bs, t->widths[symbol]);
    if (rv == ACE_BS_EOF)
        return ACE_ERR_EOF;
    *sym = symbol;
    return ACE_OK;
}

int ace_huff_from_widths(const uint8_t *widths, unsigned nwidths, unsigned max_width, ace_huff_t *out)
{
    return ace_huff_make_tree(widths, nwidths, max_width, out);
}

typedef struct {
    uint32_t w;
    int dad;
} ace_hn_t;

static void huff_limit_widths(uint8_t *len, unsigned n, unsigned max_width)
{
    unsigned i;
    uint32_t kraft = 0;
    uint32_t one;

    if (max_width == 0)
        max_width = 1;
    if (max_width > 31)
        max_width = 31;
    one = 1u << max_width;
    for (i = 0; i < n; i++) {
        if (len[i] > max_width)
            len[i] = (uint8_t)max_width;
    }
    for (i = 0; i < n; i++) {
        if (len[i])
            kraft += 1u << (max_width - len[i]);
    }
    while (kraft > one) {
        int best = -1;
        unsigned best_len = 0;
        for (i = 0; i < n; i++) {
            if (len[i] && len[i] < max_width && len[i] >= best_len) {
                best_len = len[i];
                best = (int)i;
            }
        }
        if (best < 0)
            break;
        kraft -= 1u << (max_width - len[best] - 1);
        len[best]++;
    }
    while (kraft < one) {
        int best = -1;
        unsigned best_len = 0;
        for (i = 0; i < n; i++) {
            if (len[i] > 1 && len[i] >= best_len) {
                best_len = len[i];
                best = (int)i;
            }
        }
        if (best < 0)
            break;
        kraft += 1u << (max_width - len[best]);
        len[best]--;
        if (kraft > one) {
            int long_i = -1;
            unsigned long_len = 0;
            for (i = 0; i < n; i++) {
                if (len[i] && len[i] < max_width && len[i] >= long_len) {
                    long_len = len[i];
                    long_i = (int)i;
                }
            }
            if (long_i < 0)
                break;
            kraft -= 1u << (max_width - len[long_i] - 1);
            len[long_i]++;
        }
    }
}

static void quicksort_subrange_u32(uint32_t *keys, uint16_t *values, int left, int right)
{
    int new_left = left;
    int new_right = right;
    uint32_t m = keys[right];
    while (1) {
        while (keys[new_left] > m)
            new_left++;
        while (keys[new_right] < m)
            new_right--;
        if (new_left <= new_right) {
            uint32_t tk = keys[new_left]; keys[new_left] = keys[new_right]; keys[new_right] = tk;
            uint16_t tv = values[new_left]; values[new_left] = values[new_right]; values[new_right] = tv;
            new_left++;
            new_right--;
        }
        if (new_left >= new_right)
            break;
    }
    if (left < new_right) {
        if (left < new_right - 1)
            quicksort_subrange_u32(keys, values, left, new_right);
        else if (keys[left] < keys[new_right]) {
            uint32_t tk = keys[left]; keys[left] = keys[new_right]; keys[new_right] = tk;
            uint16_t tv = values[left]; values[left] = values[new_right]; values[new_right] = tv;
        }
    }
    if (right > new_left) {
        if (new_left < right - 1)
            quicksort_subrange_u32(keys, values, new_left, right);
        else if (keys[new_left] < keys[right]) {
            uint32_t tk = keys[new_left]; keys[new_left] = keys[right]; keys[right] = tk;
            uint16_t tv = values[new_left]; values[new_left] = values[right]; values[right] = tv;
        }
    }
}

static int huff_lengths(const uint32_t *freq, unsigned n, uint8_t *len, unsigned max_width)
{
    ace_hn_t *node;
    uint32_t *sorted_freq;
    uint16_t *sorted_sym;
    int *active;
    unsigned i, used = 0, nnodes, next, alive;
    unsigned leaves;

    memset(len, 0, n);
    if (n == 0)
        return ACE_ERR_PARAM;

    sorted_freq = (uint32_t *)malloc(n * sizeof(uint32_t));
    sorted_sym = (uint16_t *)malloc(n * sizeof(uint16_t));
    if (!sorted_freq || !sorted_sym) {
        free(sorted_freq);
        free(sorted_sym);
        return ACE_ERR_NOMEM;
    }
    memcpy(sorted_freq, freq, n * sizeof(uint32_t));
    for (i = 0; i < n; i++)
        sorted_sym[i] = (uint16_t)i;

    if (n > 1)
        quicksort_subrange_u32(sorted_freq, sorted_sym, 0, (int)n - 1);

    while (used < n && sorted_freq[used] > 0)
        used++;

    if (used == 0) {
        len[0] = 1;
        if (n > 1)
            len[1] = 1;
        free(sorted_freq);
        free(sorted_sym);
        return ACE_OK;
    }
    if (used == 1) {
        unsigned other = (sorted_sym[0] == 0 && n > 1) ? 1 : 0;
        len[sorted_sym[0]] = 1;
        if (n > 1)
            len[other] = 1;
        free(sorted_freq);
        free(sorted_sym);
        return ACE_OK;
    }

    leaves = used;
    nnodes = 2 * leaves - 1;
    node = (ace_hn_t *)malloc(nnodes * sizeof(*node));
    active = (int *)malloc(leaves * sizeof(int));
    if (!node || !active) {
        free(sorted_freq);
        free(sorted_sym);
        free(node);
        free(active);
        return ACE_ERR_NOMEM;
    }

    for (i = 0; i < leaves; i++) {
        node[i].w = sorted_freq[i];
        node[i].dad = -1;
        active[i] = (int)i;
    }

    next = leaves;
    alive = leaves;
    while (alive > 1) {
        int b = active[alive - 1];
        int a = active[alive - 2];
        int insert_pos;
        int idx;

        node[next].w = node[a].w + node[b].w;
        node[next].dad = -1;
        node[a].dad = (int)next;
        node[b].dad = (int)next;
        alive -= 2;

        insert_pos = 0;
        for (idx = (int)alive - 1; idx >= 0; idx--) {
            if (node[next].w < node[active[idx]].w) {
                insert_pos = idx + 1;
                break;
            }
        }
        for (idx = (int)alive; idx > insert_pos; idx--)
            active[idx] = active[idx - 1];
        active[insert_pos] = (int)next;
        alive++;
        next++;
    }

    for (i = 0; i < leaves; i++) {
        unsigned d = 0;
        int p = node[i].dad;
        while (p >= 0) {
            d++;
            p = node[p].dad;
            if (d > 64)
                break;
        }
        if (d < 1)
            d = 1;
        if (d > 255)
            d = 255;
        len[sorted_sym[i]] = (uint8_t)d;
    }

    free(node);
    free(active);
    free(sorted_freq);
    free(sorted_sym);
    huff_limit_widths(len, n, max_width);
    return ACE_OK;
}

int ace_huff_from_freq(const uint32_t *freq, unsigned n, unsigned max_width, ace_huff_t *out)
{
    uint8_t *widths;
    unsigned last = 0, i;
    int rc;

    widths = (uint8_t *)calloc(n, 1);
    if (!widths)
        return ACE_ERR_NOMEM;
    rc = huff_lengths(freq, n, widths, max_width);
    if (rc != ACE_OK) {
        free(widths);
        return rc;
    }
    for (i = 0; i < n; i++) {
        if (widths[i])
            last = i;
    }
    rc = ace_huff_make_tree(widths, last + 1, max_width, out);
    free(widths);
    return rc;
}

int ace_huff_write_symbol(ace_bsw_t *w, const ace_huff_t *t, unsigned sym)
{
    if (!t || !t->enc_codes || !t->widths || sym >= t->nwidths)
        return ACE_ERR_PARAM;
    if (t->widths[sym] == 0)
        return ACE_ERR_CORRUPT;
    return ace_bsw_write_bits(w, t->enc_codes[sym], t->widths[sym]);
}

int ace_huff_write_tree(ace_bsw_t *w, const ace_huff_t *t)
{
    unsigned n, i, lower = 0, upper, maxw = 0, minw = 16;
    uint8_t *raw;
    uint32_t *wfreq;
    ace_huff_t wt;
    int rc;

    if (!t || !t->widths || t->nwidths == 0)
        return ACE_ERR_PARAM;
    n = t->nwidths;
    if (n > 512)
        n = 512;
    for (i = 0; i < n; i++) {
        if (t->widths[i] > 0) {
            if (t->widths[i] < minw)
                minw = t->widths[i];
            if (t->widths[i] > maxw)
                maxw = t->widths[i];
        }
    }
    if (minw <= 16 && minw > 0)
        lower = minw - 1;
    else
        lower = 0;
    upper = (maxw > lower) ? (maxw - lower + 1) : 2;
    if (upper < 2)
        upper = 2;
    if (upper > 15)
        upper = 15;

    raw = (uint8_t *)malloc(n);
    wfreq = (uint32_t *)calloc(upper + 1, sizeof(uint32_t));
    if (!raw || !wfreq) {
        free(raw);
        free(wfreq);
        return ACE_ERR_NOMEM;
    }
    raw[0] = t->widths[0] ? (uint8_t)(t->widths[0] - lower) : 0;
    for (i = 1; i < n; i++) {
        unsigned cur = t->widths[i] ? (t->widths[i] - lower) : 0;
        unsigned prev = t->widths[i - 1] ? (t->widths[i - 1] - lower) : 0;
        raw[i] = (uint8_t)((cur + upper - (prev % upper)) % upper);
    }
    i = 0;
    while (i < n) {
        if (raw[i] == 0) {
            unsigned run = 0;
            while (i + run < n && raw[i + run] == 0)
                run++;
            if (run >= 4) {
                while (run >= 4) {
                    unsigned chunk = run;
                    if (chunk > 19)
                        chunk = 19;
                    wfreq[upper]++;
                    i += chunk;
                    run -= chunk;
                }
                while (run > 0) {
                    wfreq[0]++;
                    i++;
                    run--;
                }
                continue;
            }
        }
        wfreq[raw[i]]++;
        i++;
    }

    memset(&wt, 0, sizeof(wt));
    {
        uint8_t ww_buf[16];
        memset(ww_buf, 0, sizeof(ww_buf));
        rc = huff_lengths(wfreq, upper + 1, ww_buf, ACE_HUFF_MAXWIDTHWIDTH);
        if (rc == ACE_OK)
            rc = ace_huff_make_tree(ww_buf, upper + 1, ACE_HUFF_MAXWIDTHWIDTH, &wt);
    }
    if (rc != ACE_OK) {
        free(raw);
        free(wfreq);
        return rc;
    }

    rc = ace_bsw_write_bits(w, n - 1, 9);
    if (rc == ACE_OK)
        rc = ace_bsw_write_bits(w, lower, 4);
    if (rc == ACE_OK)
        rc = ace_bsw_write_bits(w, upper, 4);
    for (i = 0; rc == ACE_OK && i < upper + 1; i++) {
        uint8_t ww = (i < wt.nwidths) ? wt.widths[i] : 0;
        if (ww > ACE_HUFF_MAXWIDTHWIDTH)
            ww = ACE_HUFF_MAXWIDTHWIDTH;
        rc = ace_bsw_write_bits(w, ww, ACE_HUFF_WIDTHWIDTHBITS);
    }
    i = 0;
    while (rc == ACE_OK && i < n) {
        if (raw[i] == 0) {
            unsigned run = 0;
            while (i + run < n && raw[i + run] == 0)
                run++;
            if (run >= 4) {
                while (run >= 4 && rc == ACE_OK) {
                    unsigned chunk = run;
                    if (chunk > 19)
                        chunk = 19;
                    rc = ace_huff_write_symbol(w, &wt, upper);
                    if (rc == ACE_OK)
                        rc = ace_bsw_write_bits(w, chunk - 4, 4);
                    i += chunk;
                    run -= chunk;
                }
                while (rc == ACE_OK && run > 0) {
                    rc = ace_huff_write_symbol(w, &wt, 0);
                    i++;
                    run--;
                }
                continue;
            }
        }
        rc = ace_huff_write_symbol(w, &wt, raw[i]);
        i++;
    }

    ace_huff_free(&wt);
    free(raw);
    free(wfreq);
    return rc;
}
