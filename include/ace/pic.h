#ifndef ACE_PIC_H
#define ACE_PIC_H

#include "ace/lz77.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ace_pic ace_pic_t;

int ace_pic_init(ace_pic_t **out);
void ace_pic_free(ace_pic_t *p);
int ace_pic_reinit(ace_pic_t *p, ace_bs_t *bs);
int ace_pic_read(ace_pic_t *p, ace_bs_t *bs, size_t want_size,
                 uint8_t **out, size_t *out_n, ace_mode_t *next_mode);
int ace_pic_write_init(ace_pic_t *p, ace_bsw_t *w, int width, int planes);
int ace_pic_write(ace_pic_t *p, ace_bsw_t *w, const uint8_t *in, size_t n);
int ace_pic_write_term(ace_bsw_t *w, unsigned mode, unsigned a, unsigned b);

#ifdef __cplusplus
}
#endif

#endif
