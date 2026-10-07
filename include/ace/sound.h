#ifndef ACE_SOUND_H
#define ACE_SOUND_H

#include "ace/lz77.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACE_SOUND_RUNLENCODES  32
#define ACE_SOUND_TYPECODE     (256 + ACE_SOUND_RUNLENCODES)
#define ACE_SOUND_NUMCODES     (256 + ACE_SOUND_RUNLENCODES + 1)
#define ACE_SOUND_MAXCODEWIDTH 10
#define ACE_SOUND_MAX_CHANNELS 3
#define ACE_SOUND_MAX_MODELS   9

typedef struct ace_sound ace_sound_t;

int ace_sound_init(ace_sound_t **out);
void ace_sound_free(ace_sound_t *s);
int ace_sound_reinit(ace_sound_t *s, unsigned mode);
int ace_sound_read(ace_sound_t *s, ace_bs_t *bs, size_t want_size,
                   uint8_t **out, size_t *out_n, ace_mode_t *next_mode);
int ace_sound_write(ace_sound_t *s, ace_bsw_t *w, const uint8_t *in, size_t n);
int ace_sound_write_term(ace_sound_t *s, ace_bsw_t *w, unsigned mode, unsigned a, unsigned b);

#ifdef __cplusplus
}
#endif

#endif
