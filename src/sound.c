#include "ace/sound.h"

#include <stdlib.h>
#include <string.h>

static const unsigned ace_sound_numchannels[4] = {1, 2, 3, 3};
static const unsigned ace_sound_usechannels[4][4] = {
    {0, 0, 0, 0},
    {0, 1, 0, 1},
    {0, 1, 0, 2},
    {1, 0, 2, 0}
};

static uint8_t sound_quantizer[256];
static int sound_quantizer_ready;

static void sound_quantizer_init(void)
{
    int i;
    if (sound_quantizer_ready)
        return;
    memset(sound_quantizer, 0, sizeof(sound_quantizer));
    for (i = 1; i < 129; i++) {
        sound_quantizer[(uint8_t)(-i)] = sound_quantizer[i] = (uint8_t)ace_bit_length((unsigned)i);
    }
    sound_quantizer_ready = 1;
}

typedef struct {
    ace_huff_t trees[ACE_SOUND_MAX_MODELS];
    unsigned num_models;
    unsigned syms_to_read;
    int trees_valid;
} sound_sr_t;

typedef struct {
    sound_sr_t *sr;
    unsigned model_base_idx;
    int pred_dif_cnt[2];
    int last_pred_dif_cnt[2];
    int rar_dif_cnt[4];
    int rar_coeff[4];
    int rar_dif[9];
    unsigned byte_count;
    int last_sample;
    int last_delta;
    int adapt_model_cnt;
    int adapt_model_use;
    int get_state;
    int get_code;
    int got_mode;
    ace_mode_t mode;
} sound_ch_t;

struct ace_sound {
    unsigned mode_idx;
    unsigned num_channels;
    sound_sr_t sr;
    sound_ch_t ch[ACE_SOUND_MAX_CHANNELS];
};

static void sr_free(sound_sr_t *sr)
{
    unsigned i;
    for (i = 0; i < ACE_SOUND_MAX_MODELS; i++)
        ace_huff_free(&sr->trees[i]);
    sr->trees_valid = 0;
    sr->syms_to_read = 0;
}

static int sr_read_trees(sound_sr_t *sr, ace_bs_t *bs)
{
    unsigned i;
    uint32_t n;
    int rc;

    sr_free(sr);
    for (i = 0; i < sr->num_models; i++) {
        rc = ace_huff_read_tree(bs, ACE_SOUND_MAXCODEWIDTH, ACE_SOUND_NUMCODES, &sr->trees[i]);
        if (rc != ACE_OK)
            return rc;
    }
    n = ace_bs_read_bits(bs, 15);
    if (n == ACE_BS_EOF)
        return ACE_ERR_EOF;
    sr->syms_to_read = n;
    sr->trees_valid = 1;
    return ACE_OK;
}

static int sr_read_symbol(sound_sr_t *sr, ace_bs_t *bs, unsigned model, unsigned *sym)
{
    int rc;
    if (sr->syms_to_read == 0) {
        rc = sr_read_trees(sr, bs);
        if (rc != ACE_OK)
            return rc;
    }
    sr->syms_to_read--;
    return ace_huff_read_symbol(&sr->trees[model], bs, sym);
}

static int ch_get_symbol(sound_ch_t *ch, ace_bs_t *bs, unsigned *sym)
{
    unsigned model = (unsigned)ch->get_state << 1;
    if (model == 0)
        model += (unsigned)ch->adapt_model_use;
    model += ch->model_base_idx;
    return sr_read_symbol(ch->sr, bs, model, sym);
}

static int ch_get(sound_ch_t *ch, ace_bs_t *bs, int *sample_or_mode)
{
    int value = 0;

    ch->got_mode = 0;
    if (ch->get_state != 2) {
        unsigned code;
        int rc = ch_get_symbol(ch, bs, &code);
        if (rc != ACE_OK)
            return rc;
        ch->get_code = (int)code;
        if (ch->get_code == ACE_SOUND_TYPECODE) {
            rc = ace_mode_read(bs, &ch->mode);
            if (rc != ACE_OK)
                return rc;
            ch->got_mode = 1;
            *sample_or_mode = 0;
            return ACE_OK;
        }
    }

    if (ch->get_state == 0) {
        if (ch->get_code >= ACE_SOUND_RUNLENCODES) {
            value = ch->get_code - ACE_SOUND_RUNLENCODES;
            ch->adapt_model_cnt = (ch->adapt_model_cnt * 7 >> 3) + value;
            if (ch->adapt_model_cnt > 40)
                ch->adapt_model_use = 1;
            else
                ch->adapt_model_use = 0;
        } else {
            ch->get_state = 2;
        }
    } else if (ch->get_state == 1) {
        value = ch->get_code;
        ch->get_state = 0;
    }

    if (ch->get_state == 2) {
        if (ch->get_code == 0)
            ch->get_state = 1;
        else
            ch->get_code -= 1;
        value = 0;
    }

    if (value & 1)
        *sample_or_mode = 255 - (value >> 1);
    else
        *sample_or_mode = value >> 1;
    return ACE_OK;
}

static int ch_get_predicted(sound_ch_t *ch)
{
    int v = (8 * ch->last_sample +
             ch->rar_coeff[0] * ch->rar_dif_cnt[0] +
             ch->rar_coeff[1] * ch->rar_dif_cnt[1] +
             ch->rar_coeff[2] * ch->rar_dif_cnt[2] +
             ch->rar_coeff[3] * ch->rar_dif_cnt[3]) >> 3;
    return ace_uchar(v);
}

static int ch_rar_predict(sound_ch_t *ch)
{
    if (ch->pred_dif_cnt[0] > ch->pred_dif_cnt[1])
        return ch->last_sample;
    return ch_get_predicted(ch);
}

static void ch_rar_adjust(sound_ch_t *ch, int sample)
{
    int pred_sample;
    int pred_dif;
    int i;

    sound_quantizer_init();
    ch->byte_count++;
    pred_sample = ch_get_predicted(ch);
    pred_dif = ace_schar(pred_sample - sample) << 3;
    ch->rar_dif[0] += abs(pred_dif - ch->rar_dif_cnt[0]);
    ch->rar_dif[1] += abs(pred_dif + ch->rar_dif_cnt[0]);
    ch->rar_dif[2] += abs(pred_dif - ch->rar_dif_cnt[1]);
    ch->rar_dif[3] += abs(pred_dif + ch->rar_dif_cnt[1]);
    ch->rar_dif[4] += abs(pred_dif - ch->rar_dif_cnt[2]);
    ch->rar_dif[5] += abs(pred_dif + ch->rar_dif_cnt[2]);
    ch->rar_dif[6] += abs(pred_dif - ch->rar_dif_cnt[3]);
    ch->rar_dif[7] += abs(pred_dif + ch->rar_dif_cnt[3]);
    ch->rar_dif[8] += abs(pred_dif);

    ch->last_delta = ace_schar(sample - ch->last_sample);
    ch->pred_dif_cnt[0] += sound_quantizer[(uint8_t)(pred_dif >> 3)];
    ch->pred_dif_cnt[1] += sound_quantizer[(uint8_t)(ch->last_sample - sample)];
    ch->last_sample = sample;

    if ((ch->byte_count & 0x1F) == 0) {
        int min_dif = 0xFFFF;
        int min_dif_pos = 8;
        for (i = 8; i >= 0; i--) {
            if (ch->rar_dif[i] <= min_dif) {
                min_dif = ch->rar_dif[i];
                min_dif_pos = i;
            }
            ch->rar_dif[i] = 0;
        }
        if (min_dif_pos != 8) {
            i = min_dif_pos >> 1;
            if ((min_dif_pos & 1) == 0) {
                if (ch->rar_coeff[i] >= -16)
                    ch->rar_coeff[i] -= 1;
            } else {
                if (ch->rar_coeff[i] <= 16)
                    ch->rar_coeff[i] += 1;
            }
        }
        if ((ch->byte_count & 0xFF) == 0) {
            for (i = 0; i < 2; i++) {
                ch->pred_dif_cnt[i] -= ch->last_pred_dif_cnt[i];
                ch->last_pred_dif_cnt[i] = ch->pred_dif_cnt[i];
            }
        }
    }

    ch->rar_dif_cnt[3] = ch->rar_dif_cnt[2];
    ch->rar_dif_cnt[2] = ch->rar_dif_cnt[1];
    ch->rar_dif_cnt[1] = ch->last_delta - ch->rar_dif_cnt[0];
    ch->rar_dif_cnt[0] = ch->last_delta;
}

int ace_sound_init(ace_sound_t **out)
{
    ace_sound_t *s = (ace_sound_t *)calloc(1, sizeof(*s));
    if (!s)
        return ACE_ERR_NOMEM;
    *out = s;
    return ACE_OK;
}

void ace_sound_free(ace_sound_t *s)
{
    if (!s)
        return;
    sr_free(&s->sr);
    free(s);
}

int ace_sound_reinit(ace_sound_t *s, unsigned mode)
{
    unsigned i;

    if (mode < ACE_MODE_SOUND_8 || mode > ACE_MODE_SOUND_32B)
        return ACE_ERR_CORRUPT;
    sr_free(&s->sr);
    memset(s, 0, sizeof(*s));
    s->mode_idx = mode - ACE_MODE_SOUND_8;
    s->num_channels = ace_sound_numchannels[s->mode_idx];
    s->sr.num_models = s->num_channels * 3;
    for (i = 0; i < s->num_channels; i++) {
        s->ch[i].sr = &s->sr;
        s->ch[i].model_base_idx = 3 * i;
    }
    return ACE_OK;
}

int ace_sound_read(ace_sound_t *s, ace_bs_t *bs, size_t want_size,
                   uint8_t **out, size_t *out_n, ace_mode_t *next_mode)
{
    size_t n = want_size & 0xFFFFFFFCu;
    size_t i;
    uint8_t *chunk;
    int rc;

    memset(next_mode, 0, sizeof(*next_mode));
    *out = NULL;
    *out_n = 0;
    if (want_size == 0)
        return ACE_ERR_PARAM;
    chunk = (uint8_t *)malloc(n ? n : 1);
    if (!chunk)
        return ACE_ERR_NOMEM;

    for (i = 0; i < n; i++) {
        unsigned channel = ace_sound_usechannels[s->mode_idx][i % 4];
        int value;
        uint8_t sample;
        rc = ch_get(&s->ch[channel], bs, &value);
        if (rc != ACE_OK) {
            free(chunk);
            return rc;
        }
        if (s->ch[channel].got_mode) {
            *next_mode = s->ch[channel].mode;
            break;
        }
        sample = ace_uchar(value + ch_rar_predict(&s->ch[channel]));
        chunk[i] = sample;
        ch_rar_adjust(&s->ch[channel], ace_schar(sample));
    }
    *out = chunk;
    *out_n = i;
    return ACE_OK;
}

static unsigned sound_enc_value(int residual)
{
    unsigned u = (uint8_t)residual;
    if (u & 0x80)
        return 2u * (255u - u) + 1u;
    return 2u * u;
}

static int sound_flush_block(ace_bsw_t *w, unsigned num_models,
                             const unsigned *models, const unsigned *codes, size_t n)
{
    uint32_t freq[ACE_SOUND_MAX_MODELS][ACE_SOUND_NUMCODES];
    ace_huff_t trees[ACE_SOUND_MAX_MODELS];
    size_t i;
    unsigned m;
    int rc = ACE_OK;

    memset(freq, 0, sizeof(freq));
    memset(trees, 0, sizeof(trees));
    for (i = 0; i < n; i++)
        freq[models[i]][codes[i]]++;
    for (m = 0; m < num_models; m++) {
        rc = ace_huff_from_freq(freq[m], ACE_SOUND_NUMCODES, ACE_SOUND_MAXCODEWIDTH, &trees[m]);
        if (rc != ACE_OK)
            break;
        rc = ace_huff_write_tree(w, &trees[m]);
        if (rc != ACE_OK)
            break;
    }
    if (rc == ACE_OK)
        rc = ace_bsw_write_bits(w, (uint32_t)n, 15);
    for (i = 0; rc == ACE_OK && i < n; i++)
        rc = ace_huff_write_symbol(w, &trees[models[i]], codes[i]);
    for (m = 0; m < num_models; m++)
        ace_huff_free(&trees[m]);
    return rc;
}

static unsigned sound_zero_run(ace_sound_t *s, const uint8_t *in, size_t n, size_t i)
{
    unsigned channel = ace_sound_usechannels[s->mode_idx][i % 4];
    sound_ch_t tmp = s->ch[channel];
    unsigned run = 0;
    size_t j;

    for (j = i; j < n && run < ACE_SOUND_RUNLENCODES; j++) {
        int predict, residual;
        if (ace_sound_usechannels[s->mode_idx][j % 4] != channel)
            continue;
        predict = ch_rar_predict(&tmp);
        residual = ace_schar((int)in[j] - predict);
        if (sound_enc_value(residual) != 0)
            break;
        ch_rar_adjust(&tmp, ace_schar(in[j]));
        run++;
    }
    return run;
}

int ace_sound_write(ace_sound_t *s, ace_bsw_t *w, const uint8_t *in, size_t n)
{
    unsigned *models;
    unsigned *codes;
    unsigned rle_left[ACE_SOUND_MAX_CHANNELS];
    size_t i, nsym, off;
    int rc = ACE_OK;
    size_t cap;

    if (!s || !w)
        return ACE_ERR_PARAM;
    n &= ~(size_t)3;
    cap = n ? n : 1;
    models = (unsigned *)malloc(cap * sizeof(unsigned));
    codes = (unsigned *)malloc(cap * sizeof(unsigned));
    if (!models || !codes) {
        free(models);
        free(codes);
        return ACE_ERR_NOMEM;
    }
    memset(rle_left, 0, sizeof(rle_left));
    nsym = 0;
    for (i = 0; i < n; i++) {
        unsigned channel = ace_sound_usechannels[s->mode_idx][i % 4];
        sound_ch_t *ch = &s->ch[channel];
        int predict;
        int residual;
        unsigned enc;
        unsigned model;

        if (rle_left[channel] > 0) {
            rle_left[channel]--;
            ch_rar_adjust(ch, ace_schar(in[i]));
            continue;
        }
        predict = ch_rar_predict(ch);
        residual = ace_schar((int)in[i] - predict);
        enc = sound_enc_value(residual);
        if (ch->get_state == 1) {
            model = 2 + ch->model_base_idx;
            models[nsym] = model;
            codes[nsym] = enc;
            nsym++;
            ch->get_state = 0;
            ch_rar_adjust(ch, ace_schar(in[i]));
            continue;
        }
        if (enc == 0) {
            unsigned run = sound_zero_run(s, in, n, i);
            if (run >= 3) {
                models[nsym] = (unsigned)ch->adapt_model_use + ch->model_base_idx;
                codes[nsym] = run - 1;
                nsym++;
                rle_left[channel] = run - 1;
                ch->get_state = 1;
                ch_rar_adjust(ch, ace_schar(in[i]));
                continue;
            }
        }
        model = (unsigned)ch->adapt_model_use + ch->model_base_idx;
        models[nsym] = model;
        codes[nsym] = enc + ACE_SOUND_RUNLENCODES;
        nsym++;
        ch->adapt_model_cnt = (ch->adapt_model_cnt * 7 >> 3) + (int)enc;
        ch->adapt_model_use = ch->adapt_model_cnt > 40;
        ch_rar_adjust(ch, ace_schar(in[i]));
    }
    off = 0;
    while (rc == ACE_OK && off < nsym) {
        size_t chunk = nsym - off;
        if (chunk > 32767)
            chunk = 32767;
        rc = sound_flush_block(w, s->sr.num_models, models + off, codes + off, chunk);
        off += chunk;
    }
    free(models);
    free(codes);
    return rc;
}

int ace_sound_write_term(ace_sound_t *s, ace_bsw_t *w, unsigned mode, unsigned a, unsigned b)
{
    unsigned models[1];
    unsigned codes[1];
    int rc;

    if (!s || !w)
        return ACE_ERR_PARAM;
    models[0] = s->ch[0].model_base_idx + (unsigned)s->ch[0].adapt_model_use;
    codes[0] = ACE_SOUND_TYPECODE;
    rc = sound_flush_block(w, s->sr.num_models, models, codes, 1);
    if (rc != ACE_OK)
        return rc;
    return ace_mode_write(w, mode, a, b);
}
