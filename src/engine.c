#include "ace/engine.h"

#include <stdlib.h>
#include <string.h>

#define ACE_FILE_BLOCKSIZE 131072u

int ace_engine_init(ace_engine_t *e)
{
    int rc;
    memset(e, 0, sizeof(*e));
    rc = ace_lz77_init(&e->lz77);
    if (rc != ACE_OK)
        return rc;
    rc = ace_sound_init(&e->sound);
    if (rc != ACE_OK)
        return rc;
    rc = ace_pic_init(&e->pic);
    if (rc != ACE_OK)
        return rc;
    return ACE_OK;
}

void ace_engine_free(ace_engine_t *e)
{
    if (!e)
        return;
    ace_lz77_free(&e->lz77);
    ace_sound_free(e->sound);
    ace_pic_free(e->pic);
    e->sound = NULL;
    e->pic = NULL;
}

int ace_compress_comment(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n)
{
    uint16_t *codes;
    size_t ncodes = 0;
    size_t i;
    int htab[511];
    uint32_t freq[ACE_LZ77_NUMMAINCODES];
    ace_huff_t tree;
    ace_bsw_t w;
    int rc;

    *out = NULL;
    *out_n = 0;
    if (n > 0x7FFF)
        return ACE_ERR_PARAM;
    codes = (uint16_t *)malloc((n ? n : 1) * sizeof(uint16_t));
    if (!codes)
        return ACE_ERR_NOMEM;
    memset(htab, 0, sizeof(htab));
    memset(freq, 0, sizeof(freq));
    i = 0;
    while (i < n) {
        int source_pos = 0;
        unsigned best = 0;
        if (i > 1) {
            int hval = in[i - 1] + in[i - 2];
            source_pos = htab[hval];
            htab[hval] = (int)i;
            if (source_pos >= 0 && (size_t)source_pos < i) {
                while (best < 29 && i + best < n &&
                       (size_t)source_pos + best < i &&
                       in[i + best] == in[source_pos + best])
                    best++;
            }
        }
        if (best >= 2) {
            unsigned code = 256 + (best - 2);
            codes[ncodes++] = (uint16_t)code;
            freq[code]++;
            i += best;
        } else {
            codes[ncodes++] = in[i];
            freq[in[i]]++;
            i++;
        }
    }
    memset(&tree, 0, sizeof(tree));
    rc = ace_huff_from_freq(freq, ACE_LZ77_NUMMAINCODES, ACE_LZ77_MAXCODEWIDTH, &tree);
    if (rc != ACE_OK) {
        free(codes);
        return rc;
    }
    rc = ace_bsw_init(&w);
    if (rc == ACE_OK)
        rc = ace_bsw_write_bits(&w, (uint32_t)n, 15);
    if (rc == ACE_OK)
        rc = ace_huff_write_tree(&w, &tree);
    for (i = 0; rc == ACE_OK && i < ncodes; i++)
        rc = ace_huff_write_symbol(&w, &tree, codes[i]);
    if (rc == ACE_OK)
        rc = ace_bsw_pad32(&w);
    ace_huff_free(&tree);
    free(codes);
    if (rc != ACE_OK) {
        ace_bsw_free(&w);
        return rc;
    }
    *out = w.data;
    *out_n = w.len;
    w.data = NULL;
    ace_bsw_free(&w);
    return ACE_OK;
}

int ace_decompress_comment(const uint8_t *buf, size_t n, uint8_t **out, size_t *out_n)
{
    ace_bs_t bs;
    ace_memsrc_t src;
    ace_huff_t tree;
    uint32_t want;
    uint8_t *comment;
    int htab[511];
    size_t i;
    int rc;

    *out = NULL;
    *out_n = 0;
    memset(htab, 0, sizeof(htab));
    rc = ace_bs_from_mem(&bs, &src, buf, n, 4096);
    if (rc != ACE_OK)
        return rc;
    want = ace_bs_read_bits(&bs, 15);
    if (want == ACE_BS_EOF) {
        ace_bs_free(&bs);
        return ACE_ERR_EOF;
    }
    rc = ace_huff_read_tree(&bs, ACE_LZ77_MAXCODEWIDTH, ACE_LZ77_NUMMAINCODES, &tree);
    if (rc != ACE_OK) {
        ace_bs_free(&bs);
        return rc;
    }
    comment = (uint8_t *)malloc(want ? want : 1);
    if (!comment) {
        ace_huff_free(&tree);
        ace_bs_free(&bs);
        return ACE_ERR_NOMEM;
    }
    i = 0;
    while (i < want) {
        unsigned code;
        int source_pos = 0;
        if (i > 1) {
            int hval = comment[i - 1] + comment[i - 2];
            source_pos = htab[hval];
            htab[hval] = (int)i;
        }
        rc = ace_huff_read_symbol(&tree, &bs, &code);
        if (rc != ACE_OK) {
            free(comment);
            ace_huff_free(&tree);
            ace_bs_free(&bs);
            return rc;
        }
        if (code < 256) {
            comment[i++] = (uint8_t)code;
        } else {
            unsigned k, cnt = code - 256 + 2;
            for (k = 0; k < cnt; k++) {
                if (i >= want)
                    break;
                if ((size_t)source_pos + k >= i) {
                    free(comment);
                    ace_huff_free(&tree);
                    ace_bs_free(&bs);
                    return ACE_ERR_CORRUPT;
                }
                comment[i] = comment[source_pos + k];
                i++;
            }
        }
    }
    ace_huff_free(&tree);
    ace_bs_free(&bs);
    *out = comment;
    *out_n = i;
    return ACE_OK;
}

int ace_decompress_stored(ace_engine_t *e, FILE *fp, uint64_t filesize, size_t dicsize,
                          ace_out_cb cb, void *cb_ctx)
{
    uint8_t buf[ACE_FILE_BLOCKSIZE];
    uint64_t produced = 0;

    ace_lz77_setsize(&e->lz77, dicsize);
    while (produced < filesize) {
        size_t want = (size_t)((filesize - produced) < ACE_FILE_BLOCKSIZE
                                   ? (filesize - produced)
                                   : ACE_FILE_BLOCKSIZE);
        size_t got = fread(buf, 1, want, fp);
        if (got == 0)
            return ACE_ERR_TRUNCATED;
        ace_lz77_register(&e->lz77, buf, got);
        if (cb) {
            int rc = cb(cb_ctx, buf, got);
            if (rc != ACE_OK)
                return rc;
        }
        produced += got;
    }
    return ACE_OK;
}

int ace_decompress_lz77(ace_engine_t *e, ace_bs_t *bs, uint64_t filesize, size_t dicsize,
                        ace_out_cb cb, void *cb_ctx)
{
    uint64_t produced = 0;
    ace_lz77_setsize(&e->lz77, dicsize);
    ace_lz77_reinit(&e->lz77);
    while (produced < filesize) {
        uint8_t *chunk = NULL;
        size_t n = 0;
        ace_mode_t next;
        int rc = ace_lz77_read(&e->lz77, bs, (size_t)(filesize - produced), &chunk, &n, &next);
        if (rc != ACE_OK) {
            free(chunk);
            return rc;
        }
        if (next.present) {
            free(chunk);
            return ACE_ERR_CORRUPT;
        }
        if (cb && n) {
            rc = cb(cb_ctx, chunk, n);
            if (rc != ACE_OK) {
                free(chunk);
                return rc;
            }
        }
        produced += n;
        free(chunk);
        if (n == 0)
            break;
    }
    return ACE_OK;
}

static int exe_patch(uint8_t *outchunk, size_t n, uint64_t produced, unsigned exe_mode,
                     uint8_t *leftover, size_t *leftover_n, size_t *new_n)
{
    size_t i = 0;
    size_t cut = n;

    while (i + 4 < n) {
        if (outchunk[i] == 0xE8) {
            uint64_t pos = produced + i;
            if (exe_mode == 0) {
                uint16_t rel16 = (uint16_t)(outchunk[i + 1] | (outchunk[i + 2] << 8));
                rel16 = (uint16_t)(rel16 - (uint16_t)pos);
                outchunk[i + 1] = (uint8_t)(rel16 & 0xFF);
                outchunk[i + 2] = (uint8_t)((rel16 >> 8) & 0xFF);
                i += 3;
            } else {
                uint32_t rel32 = ace_r32(outchunk + i + 1);
                rel32 = rel32 - (uint32_t)pos;
                outchunk[i + 1] = (uint8_t)(rel32 & 0xFF);
                outchunk[i + 2] = (uint8_t)((rel32 >> 8) & 0xFF);
                outchunk[i + 3] = (uint8_t)((rel32 >> 16) & 0xFF);
                outchunk[i + 4] = (uint8_t)((rel32 >> 24) & 0xFF);
                i += 5;
            }
        } else if (outchunk[i] == 0xE9) {
            uint64_t pos = produced + i;
            uint16_t rel16 = (uint16_t)(outchunk[i + 1] | (outchunk[i + 2] << 8));
            rel16 = (uint16_t)(rel16 - (uint16_t)pos);
            outchunk[i + 1] = (uint8_t)(rel16 & 0xFF);
            outchunk[i + 2] = (uint8_t)((rel16 >> 8) & 0xFF);
            i += 3;
        } else {
            i++;
        }
    }
    while (i < n) {
        if (outchunk[i] == 0xE8 || outchunk[i] == 0xE9) {
            cut = i;
            *leftover_n = n - i;
            memcpy(leftover, outchunk + i, *leftover_n);
            break;
        }
        i++;
    }
    *new_n = cut;
    return ACE_OK;
}

int ace_decompress_blocked(ace_engine_t *e, ace_bs_t *bs, uint64_t filesize, size_t dicsize,
                           ace_out_cb cb, void *cb_ctx)
{
    uint8_t exe_leftover[8];
    size_t exe_leftover_n = 0;
    int last_delta = 0;
    ace_mode_t next_mode;
    ace_mode_t mode;
    uint64_t produced = 0;
    int mode_inited_sound = 0;
    int mode_inited_pic = 0;

    memset(&next_mode, 0, sizeof(next_mode));
    memset(&mode, 0, sizeof(mode));
    mode.present = 1;
    mode.mode = ACE_MODE_LZ77;

    ace_lz77_setsize(&e->lz77, dicsize);
    ace_lz77_reinit(&e->lz77);

    while (produced < filesize) {
        uint8_t *outchunk = NULL;
        size_t out_n = 0;
        int rc;

        if (next_mode.present) {
            if (mode.mode != next_mode.mode) {
                if (next_mode.mode >= ACE_MODE_SOUND_8 && next_mode.mode <= ACE_MODE_SOUND_32B) {
                    rc = ace_sound_reinit(e->sound, next_mode.mode);
                    if (rc != ACE_OK)
                        return rc;
                    mode_inited_sound = 1;
                } else if (next_mode.mode == ACE_MODE_PIC) {
                    rc = ace_pic_reinit(e->pic, bs);
                    if (rc != ACE_OK)
                        return rc;
                    mode_inited_pic = 1;
                }
            }
            mode = next_mode;
            memset(&next_mode, 0, sizeof(next_mode));
        }

        if (mode.mode == ACE_MODE_LZ77_DELTA) {
            uint8_t *delta = NULL;
            size_t delta_n = 0;
            size_t i;
            unsigned delta_plane, delta_plane_pos, delta_plane_size;

            while (delta_n < mode.delta_len) {
                uint8_t *chunk = NULL;
                size_t n = 0;
                ace_mode_t nm;
                rc = ace_lz77_read(&e->lz77, bs, mode.delta_len - delta_n, &chunk, &n, &nm);
                if (rc != ACE_OK) {
                    free(delta);
                    free(chunk);
                    return rc;
                }
                if (n) {
                    uint8_t *nd = (uint8_t *)realloc(delta, delta_n + n);
                    if (!nd) {
                        free(delta);
                        free(chunk);
                        return ACE_ERR_NOMEM;
                    }
                    delta = nd;
                    memcpy(delta + delta_n, chunk, n);
                    delta_n += n;
                }
                free(chunk);
                if (nm.present) {
                    if (next_mode.present) {
                        free(delta);
                        return ACE_ERR_CORRUPT;
                    }
                    next_mode = nm;
                    if (delta_n == 0)
                        break;
                }
            }
            if (delta_n == 0 && next_mode.present)
                continue;
            if (mode.delta_dist == 0) {
                free(delta);
                return ACE_ERR_CORRUPT;
            }
            for (i = 0; i < delta_n; i++) {
                delta[i] = ace_uchar(delta[i] + last_delta);
                last_delta = delta[i];
            }
            outchunk = (uint8_t *)malloc(delta_n ? delta_n : 1);
            if (!outchunk) {
                free(delta);
                return ACE_ERR_NOMEM;
            }
            delta_plane = 0;
            delta_plane_pos = 0;
            delta_plane_size = mode.delta_len / mode.delta_dist;
            out_n = 0;
            while (delta_plane_pos < delta_plane_size) {
                while (delta_plane < mode.delta_len) {
                    size_t idx = (size_t)delta_plane + delta_plane_pos;
                    if (idx < delta_n)
                        outchunk[out_n++] = delta[idx];
                    delta_plane += delta_plane_size;
                }
                delta_plane = 0;
                delta_plane_pos++;
            }
            free(delta);
        } else if (mode.mode == ACE_MODE_LZ77 || mode.mode == ACE_MODE_LZ77_EXE) {
            uint8_t *chunk = NULL;
            size_t n = 0;
            size_t need;
            ace_mode_t nm;

            if (exe_leftover_n) {
                outchunk = (uint8_t *)malloc(exe_leftover_n);
                if (!outchunk)
                    return ACE_ERR_NOMEM;
                memcpy(outchunk, exe_leftover, exe_leftover_n);
                out_n = exe_leftover_n;
                exe_leftover_n = 0;
            }
            need = (size_t)(filesize - produced - out_n);
            rc = ace_lz77_read(&e->lz77, bs, need, &chunk, &n, &nm);
            if (rc != ACE_OK) {
                free(outchunk);
                free(chunk);
                return rc;
            }
            if (n) {
                uint8_t *nd = (uint8_t *)realloc(outchunk, out_n + n);
                if (!nd) {
                    free(outchunk);
                    free(chunk);
                    return ACE_ERR_NOMEM;
                }
                outchunk = nd;
                memcpy(outchunk + out_n, chunk, n);
                out_n += n;
            }
            free(chunk);
            if (nm.present)
                next_mode = nm;
            if (mode.mode == ACE_MODE_LZ77_EXE) {
                size_t new_n = out_n;
                exe_patch(outchunk, out_n, produced, mode.exe_mode, exe_leftover, &exe_leftover_n, &new_n);
                out_n = new_n;
            }
        } else if (mode.mode >= ACE_MODE_SOUND_8 && mode.mode <= ACE_MODE_SOUND_32B) {
            ace_mode_t nm;
            if (!mode_inited_sound) {
                rc = ace_sound_reinit(e->sound, mode.mode);
                if (rc != ACE_OK)
                    return rc;
                mode_inited_sound = 1;
            }
            rc = ace_sound_read(e->sound, bs, (size_t)(filesize - produced), &outchunk, &out_n, &nm);
            if (rc != ACE_OK)
                return rc;
            if (out_n)
                ace_lz77_register(&e->lz77, outchunk, out_n);
            if (nm.present)
                next_mode = nm;
        } else if (mode.mode == ACE_MODE_PIC) {
            ace_mode_t nm;
            if (!mode_inited_pic) {
                rc = ace_pic_reinit(e->pic, bs);
                if (rc != ACE_OK)
                    return rc;
                mode_inited_pic = 1;
            }
            rc = ace_pic_read(e->pic, bs, (size_t)(filesize - produced), &outchunk, &out_n, &nm);
            if (rc != ACE_OK)
                return rc;
            if (out_n)
                ace_lz77_register(&e->lz77, outchunk, out_n);
            if (nm.present)
                next_mode = nm;
        } else {
            return ACE_ERR_CORRUPT;
        }

        if (out_n && cb) {
            rc = cb(cb_ctx, outchunk, out_n);
            if (rc != ACE_OK) {
                free(outchunk);
                return rc;
            }
        }
        produced += out_n;
        free(outchunk);
        if (out_n == 0 && !next_mode.present)
            break;
    }
    return ACE_OK;
}
