#ifndef ACE_LZ77_H
#define ACE_LZ77_H

#include "ace/bitstream.h"
#include "ace/huffman.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACE_LZ77_MAXCODEWIDTH  11
#define ACE_LZ77_MAXLEN        259
#define ACE_LZ77_MAXDISTATLEN2 255
#define ACE_LZ77_MAXDISTATLEN3 8191
#define ACE_LZ77_MINDICBITS    10
#define ACE_LZ77_MAXDICBITS    22
#define ACE_LZ77_MINDICSIZE    (1u << ACE_LZ77_MINDICBITS)
#define ACE_LZ77_MAXDICSIZE    (1u << ACE_LZ77_MAXDICBITS)
#define ACE_LZ77_TYPECODE      (260 + ACE_LZ77_MAXDICBITS + 1)
#define ACE_LZ77_NUMMAINCODES  (260 + ACE_LZ77_MAXDICBITS + 2)
#define ACE_LZ77_NUMLENCODES   256

#define ACE_MODE_LZ77       0
#define ACE_MODE_LZ77_DELTA 1
#define ACE_MODE_LZ77_EXE   2
#define ACE_MODE_SOUND_8    3
#define ACE_MODE_SOUND_16   4
#define ACE_MODE_SOUND_32A  5
#define ACE_MODE_SOUND_32B  6
#define ACE_MODE_PIC        7

typedef struct {
    int present;
    unsigned mode;
    unsigned delta_dist;
    unsigned delta_len;
    unsigned exe_mode;
} ace_mode_t;

int ace_mode_read(ace_bs_t *bs, ace_mode_t *mode);
int ace_mode_write(ace_bsw_t *w, unsigned mode, unsigned a, unsigned b);

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    size_t dicsize;
    size_t maxsize;
} ace_dict_t;

typedef struct {
    ace_huff_t main_tree;
    ace_huff_t len_tree;
    unsigned syms_to_read;
    int trees_valid;
} ace_lz77_symreader_t;

typedef struct {
    uint32_t hist[4];
} ace_disthist_t;

typedef struct {
    ace_dict_t dict;
    ace_lz77_symreader_t sr;
    ace_disthist_t dh;
    uint8_t leftover[ACE_LZ77_MAXLEN + 16];
    size_t leftover_len;
} ace_lz77_t;

int ace_lz77_init(ace_lz77_t *z);
void ace_lz77_free(ace_lz77_t *z);
void ace_lz77_reinit(ace_lz77_t *z);
void ace_lz77_setsize(ace_lz77_t *z, size_t dicsize);
int ace_lz77_register(ace_lz77_t *z, const uint8_t *buf, size_t n);
int ace_lz77_read(ace_lz77_t *z, ace_bs_t *bs, size_t want_size,
                  uint8_t **out, size_t *out_n, ace_mode_t *next_mode);

#ifdef __cplusplus
}
#endif

#endif
