#include "ace/bitstream.h"

#include <stdlib.h>
#include <string.h>

#define ACE_BS_DEFAULT_BUFSZ 131072u

static void ace_bs_refill_buf(ace_bs_t *bs)
{
    size_t n;

    if (bs->hit_eof)
        return;
    {
        uint8_t *raw = (uint8_t *)bs->buf;
        size_t i;
        n = bs->read(bs->read_ctx, raw, bs->bufsz);
        if (n % 4 != 0) {
            bs->err = ACE_ERR_CORRUPT;
            bs->hit_eof = 1;
            bs->bufend = bs->buf;
            bs->bufptr = bs->buf;
            return;
        }
        for (i = 0; i < n; i += 4) {
            uint32_t w = (uint32_t)raw[i] | ((uint32_t)raw[i + 1] << 8) |
                         ((uint32_t)raw[i + 2] << 16) | ((uint32_t)raw[i + 3] << 24);
            bs->buf[i / 4] = w;
        }
        bs->bufend = bs->buf + (n / 4);
        bs->bufptr = bs->buf;
        if (n < bs->bufsz)
            bs->hit_eof = 1;
    }
}

static void ace_bs_refill_bits(ace_bs_t *bs)
{
    if (bs->bitcount > 32)
        return;
    if (bs->bufptr == bs->bufend) {
        ace_bs_refill_buf(bs);
        if (bs->bufptr == bs->bufend)
            return;
    }
    bs->bits |= ((uint64_t)*bs->bufptr) << (32 - bs->bitcount);
    bs->bitcount += 32;
    bs->bufptr++;
}

int ace_bs_init(ace_bs_t *bs, ace_read_cb read, void *ctx, size_t bufsz)
{
    if (!bs || !read)
        return ACE_ERR_PARAM;
    if (bufsz == 0)
        bufsz = ACE_BS_DEFAULT_BUFSZ;
    if (bufsz % 4)
        bufsz += 4 - (bufsz % 4);
    memset(bs, 0, sizeof(*bs));
    bs->read = read;
    bs->read_ctx = ctx;
    bs->bufsz = bufsz;
    bs->buf = (uint32_t *)malloc(bufsz);
    if (!bs->buf)
        return ACE_ERR_NOMEM;
    bs->bufend = bs->buf + (bufsz / sizeof(uint32_t));
    bs->bufptr = bs->bufend;
    ace_bs_refill_bits(bs);
    return ACE_OK;
}

void ace_bs_free(ace_bs_t *bs)
{
    if (!bs)
        return;
    free(bs->buf);
    bs->buf = NULL;
}

uint32_t ace_bs_peek_bits(ace_bs_t *bs, unsigned n)
{
    if (n == 0 || n >= 32)
        return 0;
    if (bs->bitcount < n)
        ace_bs_refill_bits(bs);
    return (uint32_t)(bs->bits >> (64 - n));
}

uint32_t ace_bs_skip_bits(ace_bs_t *bs, unsigned n)
{
    if (n == 0 || n >= 32)
        return ACE_BS_EOF;
    if (bs->bitcount < n) {
        ace_bs_refill_bits(bs);
        if (bs->bitcount < n)
            return ACE_BS_EOF;
    }
    bs->bits <<= n;
    bs->bitcount -= n;
    return 0;
}

uint32_t ace_bs_read_bits(ace_bs_t *bs, unsigned n)
{
    uint32_t value;
    uint32_t rv;

    value = ace_bs_peek_bits(bs, n);
    rv = ace_bs_skip_bits(bs, n);
    if (rv == ACE_BS_EOF)
        return ACE_BS_EOF;
    return value;
}

int ace_bs_read_golomb_rice(ace_bs_t *bs, unsigned r_bits, int signed_val, int *out)
{
    uint32_t value = 0;
    uint32_t bit;

    if (r_bits > 0) {
        value = ace_bs_read_bits(bs, r_bits);
        if (value == ACE_BS_EOF)
            return ACE_ERR_EOF;
    }
    for (;;) {
        bit = ace_bs_read_bits(bs, 1);
        if (bit == ACE_BS_EOF)
            return ACE_ERR_EOF;
        if (bit == 0)
            break;
        value += 1u << r_bits;
    }
    if (!signed_val) {
        *out = (int)value;
        return ACE_OK;
    }
    if (value & 1)
        *out = -(int)(value >> 1) - 1;
    else
        *out = (int)(value >> 1);
    return ACE_OK;
}

uint32_t ace_bs_read_knownwidth_uint(ace_bs_t *bs, unsigned bits)
{
    uint32_t v;

    if (bits < 2)
        return bits;
    bits -= 1;
    v = ace_bs_read_bits(bs, bits);
    if (v == ACE_BS_EOF)
        return ACE_BS_EOF;
    return v + (1u << bits);
}

size_t ace_memsrc_read(void *ctx, void *buf, size_t n)
{
    ace_memsrc_t *s = (ace_memsrc_t *)ctx;
    size_t avail = (size_t)(s->end - s->p);
    size_t take = n < avail ? n : avail;
    size_t aligned = take & ~(size_t)3;

    if (aligned)
        memcpy(buf, s->p, aligned);
    s->p += aligned;
    return aligned;
}

size_t ace_filesrc_read(void *ctx, void *buf, size_t n)
{
    ace_filesrc_t *s = (ace_filesrc_t *)ctx;
    size_t want;
    size_t got;

    if (s->remain == 0)
        return 0;
    want = n;
    if ((uint64_t)want > s->remain)
        want = (size_t)s->remain;
    want &= ~(size_t)3;
    if (want == 0)
        return 0;
    got = fread(buf, 1, want, s->fp);
    s->remain -= got;
    return got;
}

int ace_bs_from_mem(ace_bs_t *bs, ace_memsrc_t *src, const void *data, size_t n, size_t bufsz)
{
    src->p = (const uint8_t *)data;
    src->end = src->p + n;
    return ace_bs_init(bs, ace_memsrc_read, src, bufsz);
}

int ace_bs_from_file(ace_bs_t *bs, ace_filesrc_t *src, FILE *fp, uint64_t remain, size_t bufsz)
{
    src->fp = fp;
    src->remain = remain;
    return ace_bs_init(bs, ace_filesrc_read, src, bufsz);
}

int ace_bsw_init(ace_bsw_t *w)
{
    if (!w)
        return ACE_ERR_PARAM;
    memset(w, 0, sizeof(*w));
    w->cap = 4096;
    w->data = (uint8_t *)malloc(w->cap);
    if (!w->data)
        return ACE_ERR_NOMEM;
    return ACE_OK;
}

void ace_bsw_free(ace_bsw_t *w)
{
    if (!w)
        return;
    free(w->data);
    w->data = NULL;
    w->len = w->cap = 0;
    w->acc = 0;
    w->count = 0;
}

static int bsw_grow(ace_bsw_t *w, size_t extra)
{
    uint8_t *n;
    size_t need = w->len + extra;
    if (need <= w->cap)
        return ACE_OK;
    if (w->cap == 0)
        w->cap = 4096;
    while (w->cap < need)
        w->cap *= 2;
    n = (uint8_t *)realloc(w->data, w->cap);
    if (!n)
        return ACE_ERR_NOMEM;
    w->data = n;
    return ACE_OK;
}

static int bsw_emit32(ace_bsw_t *w, uint32_t word)
{
    int rc = bsw_grow(w, 4);
    if (rc != ACE_OK)
        return rc;
    w->data[w->len++] = (uint8_t)word;
    w->data[w->len++] = (uint8_t)(word >> 8);
    w->data[w->len++] = (uint8_t)(word >> 16);
    w->data[w->len++] = (uint8_t)(word >> 24);
    return ACE_OK;
}

int ace_bsw_write_bits(ace_bsw_t *w, uint32_t val, unsigned n)
{
    uint32_t mask;
    if (n == 0)
        return ACE_OK;
    if (n >= 32)
        return ACE_ERR_PARAM;
    mask = (n == 31) ? 0x7FFFFFFFu : ((1u << n) - 1u);
    w->acc |= ((uint64_t)(val & mask)) << (64 - w->count - n);
    w->count += n;
    while (w->count >= 32) {
        int rc = bsw_emit32(w, (uint32_t)(w->acc >> 32));
        if (rc != ACE_OK)
            return rc;
        w->acc <<= 32;
        w->count -= 32;
    }
    return ACE_OK;
}

int ace_bsw_write_knownwidth_uint(ace_bsw_t *w, unsigned bits, uint32_t value)
{
    if (bits < 2)
        return ACE_OK;
    bits -= 1;
    return ace_bsw_write_bits(w, value - (1u << bits), bits);
}

int ace_bsw_write_golomb_rice(ace_bsw_t *w, unsigned r_bits, int signed_val, int value)
{
    uint32_t u;
    uint32_t q;
    int rc;

    if (signed_val) {
        if (value >= 0)
            u = ((uint32_t)value) << 1;
        else
            u = (((uint32_t)(-value - 1)) << 1) | 1u;
    } else {
        if (value < 0)
            return ACE_ERR_PARAM;
        u = (uint32_t)value;
    }
    if (r_bits >= 32)
        r_bits = 31;
    if (r_bits > 0) {
        rc = ace_bsw_write_bits(w, u, r_bits);
        if (rc != ACE_OK)
            return rc;
        q = u >> r_bits;
    } else {
        q = u;
    }
    while (q > 0) {
        unsigned chunk = q > 31 ? 31 : (unsigned)q;
        rc = ace_bsw_write_bits(w, (1u << chunk) - 1u, chunk);
        if (rc != ACE_OK)
            return rc;
        q -= chunk;
    }
    return ace_bsw_write_bits(w, 0, 1);
}

int ace_bsw_pad32(ace_bsw_t *w)
{
    if (w->count == 0)
        return ACE_OK;
    return ace_bsw_write_bits(w, 0, 32 - w->count);
}
