#ifndef ACE_BLOWFISH_H
#define ACE_BLOWFISH_H

#include "ace/util.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t p[18];
    uint32_t s[4][256];
    uint32_t lastcl;
    uint32_t lastcr;
} ace_bf_t;

int ace_bf_init(ace_bf_t *bf, const uint8_t *pwd, size_t pwd_len);
int ace_bf_decrypt(ace_bf_t *bf, uint8_t *buf, size_t n);
int ace_bf_encrypt(ace_bf_t *bf, uint8_t *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
