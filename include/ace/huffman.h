#ifndef ACE_HUFFMAN_H
#define ACE_HUFFMAN_H

#include "ace/bitstream.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACE_HUFF_WIDTHWIDTHBITS 3
#define ACE_HUFF_MAXWIDTHWIDTH  ((1u << ACE_HUFF_WIDTHWIDTHBITS) - 1)

typedef struct {
    uint16_t *codes;
    uint8_t *widths;
    uint16_t *enc_codes;
    unsigned max_width;
    unsigned ncodes;
    unsigned nwidths;
} ace_huff_t;

void ace_huff_free(ace_huff_t *t);
int ace_huff_read_tree(ace_bs_t *bs, unsigned max_width, unsigned num_codes, ace_huff_t *out);
int ace_huff_read_symbol(const ace_huff_t *t, ace_bs_t *bs, unsigned *sym);
int ace_huff_from_widths(const uint8_t *widths, unsigned nwidths, unsigned max_width, ace_huff_t *out);
int ace_huff_from_freq(const uint32_t *freq, unsigned n, unsigned max_width, ace_huff_t *out);
int ace_huff_write_tree(ace_bsw_t *w, const ace_huff_t *t);
int ace_huff_write_symbol(ace_bsw_t *w, const ace_huff_t *t, unsigned sym);

#ifdef __cplusplus
}
#endif

#endif
