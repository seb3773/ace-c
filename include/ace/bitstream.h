#ifndef ACE_BITSTREAM_H
#define ACE_BITSTREAM_H

#include "ace/util.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ACE_BS_EOF 0xFFFFFFFFu

typedef size_t (*ace_read_cb)(void *ctx, void *buf, size_t n);

typedef struct {
    ace_read_cb read;
    void *read_ctx;
    uint64_t bits;
    unsigned bitcount;
    size_t bufsz;
    uint32_t *buf;
    uint32_t *bufend;
    uint32_t *bufptr;
    int hit_eof;
    int err;
} ace_bs_t;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} ace_memsrc_t;

typedef struct {
    FILE *fp;
    uint64_t remain;
} ace_filesrc_t;

int ace_bs_init(ace_bs_t *bs, ace_read_cb read, void *ctx, size_t bufsz);
void ace_bs_free(ace_bs_t *bs);

uint32_t ace_bs_peek_bits(ace_bs_t *bs, unsigned n);
uint32_t ace_bs_skip_bits(ace_bs_t *bs, unsigned n);
uint32_t ace_bs_read_bits(ace_bs_t *bs, unsigned n);
int ace_bs_read_golomb_rice(ace_bs_t *bs, unsigned r_bits, int signed_val, int *out);
uint32_t ace_bs_read_knownwidth_uint(ace_bs_t *bs, unsigned bits);

size_t ace_memsrc_read(void *ctx, void *buf, size_t n);
size_t ace_filesrc_read(void *ctx, void *buf, size_t n);

int ace_bs_from_mem(ace_bs_t *bs, ace_memsrc_t *src, const void *data, size_t n, size_t bufsz);
int ace_bs_from_file(ace_bs_t *bs, ace_filesrc_t *src, FILE *fp, uint64_t remain, size_t bufsz);

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
    uint64_t acc;
    unsigned count;
} ace_bsw_t;

int ace_bsw_init(ace_bsw_t *w);
void ace_bsw_free(ace_bsw_t *w);
int ace_bsw_write_bits(ace_bsw_t *w, uint32_t val, unsigned n);
int ace_bsw_write_knownwidth_uint(ace_bsw_t *w, unsigned bits, uint32_t value);
int ace_bsw_write_golomb_rice(ace_bsw_t *w, unsigned r_bits, int signed_val, int value);
int ace_bsw_pad32(ace_bsw_t *w);

#ifdef __cplusplus
}
#endif

#endif
