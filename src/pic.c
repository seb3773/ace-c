#include "ace/pic.h"

#include <stdlib.h>
#include <string.h>

#define ACE_PIC_NCTX 365

typedef struct {
    int used_counter;
    int predictor_number;
    int average_counter;
    int error_counters[4];
} pic_ctx_t;

typedef struct {
    pic_ctx_t ctx[ACE_PIC_NCTX];
} pic_errmodel_t;

typedef struct {
    int pixel_a, pixel_b, pixel_c, pixel_d, pixel_x;
    int kind;
} pic_dec_t;

struct ace_pic {
    int width;
    int planes;
    pic_errmodel_t plane0;
    pic_errmodel_t plane1n;
    uint8_t *prevrow;
    uint8_t *leftover;
    size_t leftover_len;
    size_t leftover_cap;
};

static int8_t pic_quantizer[511];
static int pic_quantizer9[511];
static int pic_quantizer81[511];
static uint8_t pic_dif_bit_width[256];
static int pic_tables_ready;

static int c_div_int(int q, int d)
{
    int s;
    if (d == 0)
        return 0;
    s = ((q < 0) == (d < 0)) ? 1 : -1;
    if (q < 0)
        q = -q;
    if (d < 0)
        d = -d;
    return s * (q / d);
}

static void pic_tables_init(void)
{
    int i;
    if (pic_tables_ready)
        return;
    for (i = 0; i < 128; i++)
        pic_dif_bit_width[i] = (uint8_t)ace_bit_length((unsigned)(2 * i));
    for (i = -128; i < 0; i++)
        pic_dif_bit_width[(uint8_t)i] = (uint8_t)ace_bit_length((unsigned)(-2 * i - 1));

    i = 0;
    for (int v = -255; v < -20; v++)
        pic_quantizer[i++] = -4;
    for (int v = -20; v < -6; v++)
        pic_quantizer[i++] = -3;
    for (int v = -6; v < -2; v++)
        pic_quantizer[i++] = -2;
    for (int v = -2; v < 0; v++)
        pic_quantizer[i++] = -1;
    pic_quantizer[i++] = 0;
    for (int v = 1; v < 3; v++)
        pic_quantizer[i++] = 1;
    for (int v = 3; v < 7; v++)
        pic_quantizer[i++] = 2;
    for (int v = 7; v < 21; v++)
        pic_quantizer[i++] = 3;
    for (int v = 21; v < 256; v++)
        pic_quantizer[i++] = 4;
    for (i = 0; i < 511; i++) {
        pic_quantizer9[i] = 9 * pic_quantizer[i];
        pic_quantizer81[i] = 81 * pic_quantizer[i];
    }
    pic_tables_ready = 1;
}

static void errmodel_reset(pic_errmodel_t *m)
{
    int i;
    for (i = 0; i < ACE_PIC_NCTX; i++) {
        m->ctx[i].used_counter = 0;
        m->ctx[i].predictor_number = 0;
        m->ctx[i].average_counter = 4;
        m->ctx[i].error_counters[0] = 0;
        m->ctx[i].error_counters[1] = 0;
        m->ctx[i].error_counters[2] = 0;
        m->ctx[i].error_counters[3] = 0;
    }
}

static void dec_init0(pic_dec_t *d)
{
    memset(d, 0, sizeof(*d));
    d->kind = 0;
}

static void dec_init_diff(pic_dec_t *d, int kind)
{
    d->pixel_a = 128;
    d->pixel_b = 128;
    d->pixel_c = 128;
    d->pixel_d = 0;
    d->pixel_x = 128;
    d->kind = kind;
}

static void dec_shift(pic_dec_t *d)
{
    d->pixel_c = d->pixel_a;
    d->pixel_a = d->pixel_d;
    d->pixel_b = d->pixel_x;
}

static int dec_get_context(pic_dec_t *d)
{
    int ctx = pic_quantizer81[255 + d->pixel_d - d->pixel_a] +
              pic_quantizer9[255 + d->pixel_a - d->pixel_c] +
              pic_quantizer[255 + d->pixel_c - d->pixel_b];
    return ctx < 0 ? -ctx : ctx;
}

static int dec_predict(pic_dec_t *d, int use_predictor)
{
    if (use_predictor == 0)
        return d->pixel_a;
    if (use_predictor == 1)
        return d->pixel_b;
    if (use_predictor == 2)
        return (d->pixel_a + d->pixel_b) >> 1;
    return ace_uchar(d->pixel_a + d->pixel_b - d->pixel_c);
}

static int dec_update_x(pic_dec_t *d, ace_bs_t *bs, pic_ctx_t *context)
{
    int r, epsilon, predicted, i, best = 0;
    int rc;

    context->used_counter += 1;
    r = c_div_int(context->average_counter, context->used_counter);
    rc = ace_bs_read_golomb_rice(bs, (unsigned)ace_bit_length((unsigned)r), 1, &epsilon);
    if (rc != ACE_OK)
        return rc;
    predicted = dec_predict(d, context->predictor_number);
    d->pixel_x = ace_uchar(predicted + epsilon);

    context->average_counter += abs(epsilon);
    if (context->used_counter == 128) {
        context->used_counter >>= 1;
        context->average_counter >>= 1;
    }

    for (i = 0; i < 4; i++) {
        int dif = d->pixel_x - dec_predict(d, i);
        context->error_counters[i] += pic_dif_bit_width[(uint8_t)dif];
        if (i == 0 || context->error_counters[i] < context->error_counters[best])
            best = i;
    }
    context->predictor_number = best;
    if (context->error_counters[0] > 0x7F ||
        context->error_counters[1] > 0x7F ||
        context->error_counters[2] > 0x7F ||
        context->error_counters[3] > 0x7F) {
        for (i = 0; i < 4; i++)
            context->error_counters[i] >>= 1;
    }
    return ACE_OK;
}

static void dec_update_d(pic_dec_t *d, int thisplane_d, int refplane_d)
{
    if (d->kind == 0)
        d->pixel_d = thisplane_d;
    else if (d->kind == 1)
        d->pixel_d = ace_uchar(128 + thisplane_d - refplane_d);
    else
        d->pixel_d = ace_uchar(128 + thisplane_d - (refplane_d * 11 >> 4));
}

static int dec_produce(pic_dec_t *d, int refplane_x)
{
    if (d->kind == 0)
        return d->pixel_x;
    if (d->kind == 1)
        return ace_uchar(d->pixel_x + refplane_x - 128);
    return ace_uchar(d->pixel_x + (refplane_x * 11 >> 4) - 128);
}

int ace_pic_init(ace_pic_t **out)
{
    ace_pic_t *p = (ace_pic_t *)calloc(1, sizeof(*p));
    if (!p)
        return ACE_ERR_NOMEM;
    *out = p;
    return ACE_OK;
}

void ace_pic_free(ace_pic_t *p)
{
    if (!p)
        return;
    free(p->prevrow);
    free(p->leftover);
    free(p);
}

int ace_pic_reinit(ace_pic_t *p, ace_bs_t *bs)
{
    int w, planes, rc;
    pic_tables_init();
    rc = ace_bs_read_golomb_rice(bs, 12, 0, &w);
    if (rc != ACE_OK)
        return rc;
    rc = ace_bs_read_golomb_rice(bs, 2, 0, &planes);
    if (rc != ACE_OK)
        return rc;
    if (w <= 0 || planes <= 0)
        return ACE_ERR_CORRUPT;
    free(p->prevrow);
    free(p->leftover);
    memset(p, 0, sizeof(*p));
    p->width = w;
    p->planes = planes;
    p->prevrow = (uint8_t *)calloc((size_t)(w + planes), 1);
    if (!p->prevrow)
        return ACE_ERR_NOMEM;
    errmodel_reset(&p->plane0);
    errmodel_reset(&p->plane1n);
    return ACE_OK;
}

static int pic_row(ace_pic_t *p, ace_bs_t *bs, uint8_t *row_out)
{
    int plane, col;
    uint8_t *row;
    size_t rowsz = (size_t)(p->width + p->planes);

    row = (uint8_t *)calloc(rowsz, 1);
    if (!row)
        return ACE_ERR_NOMEM;

    for (plane = 0; plane < p->planes; plane++) {
        pic_errmodel_t *errmodel;
        pic_dec_t decoder;
        int ref_m1;

        if (plane == 0) {
            errmodel = &p->plane0;
            dec_init0(&decoder);
        } else {
            uint32_t kind;
            errmodel = &p->plane1n;
            kind = ace_bs_read_bits(bs, 2);
            if (kind == ACE_BS_EOF) {
                free(row);
                return ACE_ERR_EOF;
            }
            if (kind > 2) {
                free(row);
                return ACE_ERR_CORRUPT;
            }
            if (kind == 0)
                dec_init0(&decoder);
            else
                dec_init_diff(&decoder, (int)kind);
        }
        ref_m1 = (plane - 1 < 0) ? 0 : p->prevrow[plane - 1];
        dec_update_d(&decoder, p->prevrow[plane], ref_m1);

        for (col = plane; col < p->width; col += p->planes) {
            int ctx_i;
            int rc;
            int ref_x;
            dec_shift(&decoder);
            dec_update_d(&decoder,
                         p->prevrow[col + p->planes],
                         p->prevrow[col + p->planes - 1]);
            ctx_i = dec_get_context(&decoder);
            if (ctx_i < 0 || ctx_i >= ACE_PIC_NCTX) {
                free(row);
                return ACE_ERR_CORRUPT;
            }
            rc = dec_update_x(&decoder, bs, &errmodel->ctx[ctx_i]);
            if (rc != ACE_OK) {
                free(row);
                return rc;
            }
            ref_x = (col - 1 < 0) ? 0 : row[col - 1];
            row[col] = (uint8_t)dec_produce(&decoder, ref_x);
        }
    }
    memcpy(p->prevrow, row, rowsz);
    memcpy(row_out, row, (size_t)p->width);
    free(row);
    return ACE_OK;
}

int ace_pic_read(ace_pic_t *p, ace_bs_t *bs, size_t want_size,
                 uint8_t **out, size_t *out_n, ace_mode_t *next_mode)
{
    uint8_t *chunk;
    size_t cap;
    size_t len = 0;
    int rc;

    memset(next_mode, 0, sizeof(*next_mode));
    *out = NULL;
    *out_n = 0;
    if (want_size == 0)
        return ACE_ERR_PARAM;

    cap = want_size;
    chunk = (uint8_t *)malloc(cap ? cap : 1);
    if (!chunk)
        return ACE_ERR_NOMEM;

    if (p->leftover_len > 0) {
        size_t n = p->leftover_len < want_size ? p->leftover_len : want_size;
        memcpy(chunk, p->leftover, n);
        len = n;
        if (n < p->leftover_len)
            memmove(p->leftover, p->leftover + n, p->leftover_len - n);
        p->leftover_len -= n;
    }

    while (len < want_size) {
        uint32_t bit = ace_bs_read_bits(bs, 1);
        uint8_t *row;
        size_t n;
        if (bit == ACE_BS_EOF) {
            free(chunk);
            return ACE_ERR_EOF;
        }
        if (bit == 0) {
            rc = ace_mode_read(bs, next_mode);
            if (rc != ACE_OK) {
                free(chunk);
                return rc;
            }
            break;
        }
        row = (uint8_t *)malloc((size_t)p->width);
        if (!row) {
            free(chunk);
            return ACE_ERR_NOMEM;
        }
        rc = pic_row(p, bs, row);
        if (rc != ACE_OK) {
            free(row);
            free(chunk);
            return rc;
        }
        n = want_size - len;
        if (n > (size_t)p->width)
            n = (size_t)p->width;
        memcpy(chunk + len, row, n);
        len += n;
        if (n < (size_t)p->width) {
            size_t left = (size_t)p->width - n;
            if (p->leftover_cap < left) {
                uint8_t *nl = (uint8_t *)realloc(p->leftover, left);
                if (!nl) {
                    free(row);
                    free(chunk);
                    return ACE_ERR_NOMEM;
                }
                p->leftover = nl;
                p->leftover_cap = left;
            }
            memcpy(p->leftover, row + n, left);
            p->leftover_len = left;
        }
        free(row);
    }
    *out = chunk;
    *out_n = len;
    return ACE_OK;
}

int ace_pic_write_init(ace_pic_t *p, ace_bsw_t *w, int width, int planes)
{
    int rc;
    pic_tables_init();
    if (!p || !w || width <= 0 || planes <= 0)
        return ACE_ERR_PARAM;
    free(p->prevrow);
    free(p->leftover);
    memset(p, 0, sizeof(*p));
    p->width = width;
    p->planes = planes;
    p->prevrow = (uint8_t *)calloc((size_t)(width + planes), 1);
    if (!p->prevrow)
        return ACE_ERR_NOMEM;
    errmodel_reset(&p->plane0);
    errmodel_reset(&p->plane1n);
    rc = ace_bsw_write_golomb_rice(w, 12, 0, width);
    if (rc == ACE_OK)
        rc = ace_bsw_write_golomb_rice(w, 2, 0, planes);
    return rc;
}

static int pic_encode_x(pic_dec_t *d, ace_bsw_t *w, pic_ctx_t *context, int pixel)
{
    int r, epsilon, predicted, i, best = 0;

    context->used_counter += 1;
    r = c_div_int(context->average_counter, context->used_counter);
    predicted = dec_predict(d, context->predictor_number);
    epsilon = ace_schar(pixel - predicted);
    {
        int rc = ace_bsw_write_golomb_rice(w, (unsigned)ace_bit_length((unsigned)r), 1, epsilon);
        if (rc != ACE_OK)
            return rc;
    }
    d->pixel_x = ace_uchar(predicted + epsilon);
    context->average_counter += abs(epsilon);
    if (context->used_counter == 128) {
        context->used_counter >>= 1;
        context->average_counter >>= 1;
    }
    for (i = 0; i < 4; i++) {
        int dif = d->pixel_x - dec_predict(d, i);
        context->error_counters[i] += pic_dif_bit_width[(uint8_t)dif];
        if (i == 0 || context->error_counters[i] < context->error_counters[best])
            best = i;
    }
    context->predictor_number = best;
    if (context->error_counters[0] > 0x7F ||
        context->error_counters[1] > 0x7F ||
        context->error_counters[2] > 0x7F ||
        context->error_counters[3] > 0x7F) {
        for (i = 0; i < 4; i++)
            context->error_counters[i] >>= 1;
    }
    return ACE_OK;
}

static int pic_write_row(ace_pic_t *p, ace_bsw_t *w, const uint8_t *src)
{
    int plane, col;
    uint8_t *row;
    size_t rowsz = (size_t)(p->width + p->planes);

    row = (uint8_t *)calloc(rowsz, 1);
    if (!row)
        return ACE_ERR_NOMEM;
    memcpy(row, src, (size_t)p->width);

    for (plane = 0; plane < p->planes; plane++) {
        pic_errmodel_t *errmodel;
        pic_dec_t decoder;
        int ref_m1;

        int plane_kind = 0;
        if (plane == 0) {
            errmodel = &p->plane0;
            dec_init0(&decoder);
        } else {
            int e0 = 0, e1 = 0, e2 = 0;
            int rc;
            for (col = plane; col < p->width; col += p->planes) {
                int ref_x = (col - 1 < 0) ? 0 : row[col - 1];
                int px = row[col];
                int same = (col >= p->planes) ? row[col - p->planes] : 0;
                e0 += abs(ace_schar(px - same));
                e1 += abs(ace_schar(px - ref_x));
                e2 += abs(ace_schar(px - (ref_x * 11 >> 4)));
            }
            plane_kind = 0;
            if (e1 < e0 && e1 <= e2)
                plane_kind = 1;
            else if (e2 < e0 && e2 < e1)
                plane_kind = 2;
            rc = ace_bsw_write_bits(w, (uint32_t)plane_kind, 2);
            if (rc != ACE_OK) {
                free(row);
                return rc;
            }
            errmodel = &p->plane1n;
            if (plane_kind == 0)
                dec_init0(&decoder);
            else
                dec_init_diff(&decoder, plane_kind);
        }
        ref_m1 = (plane - 1 < 0) ? 0 : p->prevrow[plane - 1];
        dec_update_d(&decoder, p->prevrow[plane], ref_m1);
        for (col = plane; col < p->width; col += p->planes) {
            int ctx_i;
            int rc;
            int ref_x;
            int target;
            dec_shift(&decoder);
            dec_update_d(&decoder,
                         p->prevrow[col + p->planes],
                         p->prevrow[col + p->planes - 1]);
            ctx_i = dec_get_context(&decoder);
            if (ctx_i < 0 || ctx_i >= ACE_PIC_NCTX) {
                free(row);
                return ACE_ERR_CORRUPT;
            }
            ref_x = (col - 1 < 0) ? 0 : row[col - 1];
            target = row[col];
            if (plane_kind == 1)
                target = ace_uchar(row[col] - ref_x + 128);
            else if (plane_kind == 2)
                target = ace_uchar(row[col] - (ref_x * 11 >> 4) + 128);
            rc = pic_encode_x(&decoder, w, &errmodel->ctx[ctx_i], target);
            if (rc != ACE_OK) {
                free(row);
                return rc;
            }
        }
    }
    memcpy(p->prevrow, row, rowsz);
    free(row);
    return ACE_OK;
}

int ace_pic_write(ace_pic_t *p, ace_bsw_t *w, const uint8_t *in, size_t n)
{
    size_t off = 0;
    int rc;

    if (!p || !w || p->width <= 0)
        return ACE_ERR_PARAM;
    while (off + (size_t)p->width <= n) {
        rc = ace_bsw_write_bits(w, 1, 1);
        if (rc != ACE_OK)
            return rc;
        rc = pic_write_row(p, w, in + off);
        if (rc != ACE_OK)
            return rc;
        off += (size_t)p->width;
    }
    return ACE_OK;
}

int ace_pic_write_term(ace_bsw_t *w, unsigned mode, unsigned a, unsigned b)
{
    int rc;
    if (!w)
        return ACE_ERR_PARAM;
    rc = ace_bsw_write_bits(w, 0, 1);
    if (rc != ACE_OK)
        return rc;
    return ace_mode_write(w, mode, a, b);
}
