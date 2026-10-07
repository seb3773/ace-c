#ifndef ACE_COMPRESS_H
#define ACE_COMPRESS_H

#include "ace/engine.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *data;
    size_t n;
    size_t cap;
} ace_comp_hist_t;

int ace_comp_hist_init(ace_comp_hist_t *h);
void ace_comp_hist_free(ace_comp_hist_t *h);
int ace_comp_hist_add(ace_comp_hist_t *h, const uint8_t *p, size_t n, size_t dicsize);

int ace_compress_lz77(const uint8_t *in, size_t n, size_t dicsize,
                      unsigned quality, uint8_t **out, size_t *out_n);
int ace_compress_lz77_hist(const uint8_t *in, size_t n, size_t dicsize,
                           unsigned quality, ace_comp_hist_t *hist,
                           uint8_t **out, size_t *out_n);
int ace_compress_blocked(const uint8_t *in, size_t n, size_t dicsize,
                         unsigned quality, uint8_t **out, size_t *out_n);
int ace_compress_blocked_ex(const uint8_t *in, size_t n, size_t dicsize,
                            unsigned quality, unsigned force_mode,
                            int pic_width, int pic_planes,
                            uint8_t **out, size_t *out_n);
int ace_compress_blocked_hist(const uint8_t *in, size_t n, size_t dicsize,
                              unsigned quality, unsigned force_mode,
                              int pic_width, int pic_planes,
                              ace_comp_hist_t *hist,
                              uint8_t **out, size_t *out_n);

#ifdef __cplusplus
}
#endif

#endif
