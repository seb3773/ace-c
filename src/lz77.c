#include "ace/lz77.h"

#include <stdlib.h>
#include <string.h>

int ace_mode_read(ace_bs_t *bs, ace_mode_t *mode)
{
    uint32_t m, a, b;

    memset(mode, 0, sizeof(*mode));
    m = ace_bs_read_bits(bs, 8);
    if (m == ACE_BS_EOF)
        return ACE_ERR_EOF;
    mode->present = 1;
    mode->mode = m;
    if (m == ACE_MODE_LZ77_DELTA) {
        a = ace_bs_read_bits(bs, 8);
        b = ace_bs_read_bits(bs, 17);
        if (a == ACE_BS_EOF || b == ACE_BS_EOF)
            return ACE_ERR_EOF;
        mode->delta_dist = a;
        mode->delta_len = b;
        if (mode->delta_dist == 0)
            return ACE_ERR_CORRUPT;
    } else if (m == ACE_MODE_LZ77_EXE) {
        a = ace_bs_read_bits(bs, 8);
        if (a == ACE_BS_EOF)
            return ACE_ERR_EOF;
        mode->exe_mode = a;
    }
    return ACE_OK;
}

int ace_mode_write(ace_bsw_t *w, unsigned mode, unsigned a, unsigned b)
{
    int rc = ace_bsw_write_bits(w, mode, 8);
    if (rc != ACE_OK)
        return rc;
    if (mode == ACE_MODE_LZ77_DELTA) {
        rc = ace_bsw_write_bits(w, a, 8);
        if (rc == ACE_OK)
            rc = ace_bsw_write_bits(w, b, 17);
    } else if (mode == ACE_MODE_LZ77_EXE) {
        rc = ace_bsw_write_bits(w, a, 8);
    }
    return rc;
}

static int dict_reserve(ace_dict_t *d, size_t extra)
{
    size_t need = d->len + extra;
    uint8_t *nd;

    if (need <= d->cap)
        return ACE_OK;
    if (d->cap == 0)
        d->cap = 65536;
    while (d->cap < need)
        d->cap *= 2;
    nd = (uint8_t *)realloc(d->data, d->cap);
    if (!nd)
        return ACE_ERR_NOMEM;
    d->data = nd;
    return ACE_OK;
}

static void dict_truncate(ace_dict_t *d)
{
    if (d->len > 8 * d->dicsize) {
        memmove(d->data, d->data + (d->len - d->dicsize), d->dicsize);
        d->len = d->dicsize;
    }
}

int ace_lz77_init(ace_lz77_t *z)
{
    memset(z, 0, sizeof(*z));
    z->dict.dicsize = ACE_LZ77_MINDICSIZE;
    z->dict.maxsize = ACE_LZ77_MAXDICSIZE;
    return ACE_OK;
}

void ace_lz77_free(ace_lz77_t *z)
{
    if (!z)
        return;
    free(z->dict.data);
    ace_huff_free(&z->sr.main_tree);
    ace_huff_free(&z->sr.len_tree);
    memset(z, 0, sizeof(*z));
}

void ace_lz77_reinit(ace_lz77_t *z)
{
    ace_huff_free(&z->sr.main_tree);
    ace_huff_free(&z->sr.len_tree);
    memset(&z->sr, 0, sizeof(z->sr));
    memset(&z->dh, 0, sizeof(z->dh));
    z->leftover_len = 0;
}

void ace_lz77_setsize(ace_lz77_t *z, size_t dicsize)
{
    if (dicsize < z->dict.dicsize)
        dicsize = z->dict.dicsize;
    if (dicsize > z->dict.maxsize)
        dicsize = z->dict.maxsize;
    z->dict.dicsize = dicsize;
}

int ace_lz77_register(ace_lz77_t *z, const uint8_t *buf, size_t n)
{
    int rc = dict_reserve(&z->dict, n);
    if (rc != ACE_OK)
        return rc;
    memcpy(z->dict.data + z->dict.len, buf, n);
    z->dict.len += n;
    dict_truncate(&z->dict);
    return ACE_OK;
}

static int sr_read_trees(ace_lz77_symreader_t *sr, ace_bs_t *bs)
{
    uint32_t n;
    int rc;

    ace_huff_free(&sr->main_tree);
    ace_huff_free(&sr->len_tree);
    rc = ace_huff_read_tree(bs, ACE_LZ77_MAXCODEWIDTH, ACE_LZ77_NUMMAINCODES, &sr->main_tree);
    if (rc != ACE_OK)
        return rc;
    rc = ace_huff_read_tree(bs, ACE_LZ77_MAXCODEWIDTH, ACE_LZ77_NUMLENCODES - 1, &sr->len_tree);
    if (rc != ACE_OK)
        return rc;
    n = ace_bs_read_bits(bs, 15);
    if (n == ACE_BS_EOF)
        return ACE_ERR_EOF;
    sr->syms_to_read = n;
    sr->trees_valid = 1;
    return ACE_OK;
}

static int sr_read_main(ace_lz77_symreader_t *sr, ace_bs_t *bs, unsigned *sym)
{
    int rc;
    if (sr->syms_to_read == 0) {
        rc = sr_read_trees(sr, bs);
        if (rc != ACE_OK)
            return rc;
    }
    sr->syms_to_read--;
    return ace_huff_read_symbol(&sr->main_tree, bs, sym);
}

static uint32_t dh_retrieve(ace_disthist_t *dh, unsigned offset)
{
    unsigned idx = 4 - offset - 1;
    uint32_t dist = dh->hist[idx];
    memmove(&dh->hist[idx], &dh->hist[idx + 1], (3 - idx) * sizeof(uint32_t));
    dh->hist[3] = dist;
    return dist;
}

static void dh_append(ace_disthist_t *dh, uint32_t dist)
{
    memmove(&dh->hist[0], &dh->hist[1], 3 * sizeof(uint32_t));
    dh->hist[3] = dist;
}

int ace_lz77_read(ace_lz77_t *z, ace_bs_t *bs, size_t want_size,
                  uint8_t **out, size_t *out_n, ace_mode_t *next_mode)
{
    size_t have = 0;
    size_t start;
    int rc;

    memset(next_mode, 0, sizeof(*next_mode));
    *out = NULL;
    *out_n = 0;
    if (want_size == 0)
        return ACE_ERR_PARAM;

    if (z->leftover_len > 0) {
        rc = dict_reserve(&z->dict, z->leftover_len);
        if (rc != ACE_OK)
            return rc;
        memcpy(z->dict.data + z->dict.len, z->leftover, z->leftover_len);
        z->dict.len += z->leftover_len;
        have += z->leftover_len;
        z->leftover_len = 0;
    }

    start = z->dict.len - have;
    while (have < want_size) {
        unsigned symbol;
        rc = sr_read_main(&z->sr, bs, &symbol);
        if (rc != ACE_OK)
            return rc;
        if (symbol <= 255) {
            rc = dict_reserve(&z->dict, 1);
            if (rc != ACE_OK)
                return rc;
            z->dict.data[z->dict.len++] = (uint8_t)symbol;
            have++;
        } else if (symbol < ACE_LZ77_TYPECODE) {
            unsigned copy_len;
            uint32_t copy_dist;
            size_t src;
            size_t i;

            if (symbol <= 259) {
                unsigned offset = symbol & 3;
                rc = ace_huff_read_symbol(&z->sr.len_tree, bs, &copy_len);
                if (rc != ACE_OK)
                    return rc;
                copy_dist = dh_retrieve(&z->dh, offset);
                if (offset > 1)
                    copy_len += 3;
                else
                    copy_len += 2;
            } else {
                copy_dist = ace_bs_read_knownwidth_uint(bs, symbol - 260);
                if (copy_dist == ACE_BS_EOF)
                    return ACE_ERR_EOF;
                rc = ace_huff_read_symbol(&z->sr.len_tree, bs, &copy_len);
                if (rc != ACE_OK)
                    return rc;
                dh_append(&z->dh, copy_dist);
                if (copy_dist <= ACE_LZ77_MAXDISTATLEN2)
                    copy_len += 2;
                else if (copy_dist <= ACE_LZ77_MAXDISTATLEN3)
                    copy_len += 3;
                else
                    copy_len += 4;
            }
            copy_dist += 1;
            if (have + copy_len > want_size)
                return ACE_ERR_CORRUPT;
            if (z->dict.len < copy_dist)
                return ACE_ERR_CORRUPT;
            rc = dict_reserve(&z->dict, copy_len);
            if (rc != ACE_OK)
                return rc;
            src = z->dict.len - copy_dist;
            for (i = 0; i < copy_len; i++)
                z->dict.data[z->dict.len + i] = z->dict.data[src + i];
            z->dict.len += copy_len;
            have += copy_len;
        } else if (symbol == ACE_LZ77_TYPECODE) {
            rc = ace_mode_read(bs, next_mode);
            if (rc != ACE_OK)
                return rc;
            break;
        } else {
            return ACE_ERR_CORRUPT;
        }
    }

    start = z->dict.len - have;
    *out = (uint8_t *)malloc(have ? have : 1);
    if (!*out)
        return ACE_ERR_NOMEM;
    if (have)
        memcpy(*out, z->dict.data + start, have);
    *out_n = have;
    dict_truncate(&z->dict);
    return ACE_OK;
}
