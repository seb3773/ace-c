#ifndef ACE_CRC_H
#define ACE_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t state;
} ace_crc32_t;

void ace_crc32_init(ace_crc32_t *c);
void ace_crc32_update(ace_crc32_t *c, const void *buf, size_t n);
uint32_t ace_crc32_final(const ace_crc32_t *c);
uint32_t ace_crc32(const void *buf, size_t n);
uint16_t ace_crc16(const void *buf, size_t n);

#ifdef __cplusplus
}
#endif

#endif
