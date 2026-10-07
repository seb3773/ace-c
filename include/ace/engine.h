#ifndef ACE_ENGINE_H
#define ACE_ENGINE_H

#include "ace/lz77.h"
#include "ace/sound.h"
#include "ace/pic.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACE_COMP_STORED  0
#define ACE_COMP_LZ77    1
#define ACE_COMP_BLOCKED 2

typedef int (*ace_out_cb)(void *ctx, const uint8_t *buf, size_t n);

typedef struct {
    ace_lz77_t lz77;
    ace_sound_t *sound;
    ace_pic_t *pic;
} ace_engine_t;

int ace_engine_init(ace_engine_t *e);
void ace_engine_free(ace_engine_t *e);

int ace_decompress_comment(const uint8_t *buf, size_t n, uint8_t **out, size_t *out_n);
int ace_compress_comment(const uint8_t *in, size_t n, uint8_t **out, size_t *out_n);

int ace_decompress_stored(ace_engine_t *e, FILE *fp, uint64_t filesize, size_t dicsize,
                          ace_out_cb cb, void *cb_ctx);
int ace_decompress_lz77(ace_engine_t *e, ace_bs_t *bs, uint64_t filesize, size_t dicsize,
                        ace_out_cb cb, void *cb_ctx);
int ace_decompress_blocked(ace_engine_t *e, ace_bs_t *bs, uint64_t filesize, size_t dicsize,
                           ace_out_cb cb, void *cb_ctx);

#ifdef __cplusplus
}
#endif

#endif
