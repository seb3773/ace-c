#ifndef ACE_UTIL_H
#define ACE_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACE_OK = 0,
    ACE_ERR_IO = -1,
    ACE_ERR_NOMEM = -2,
    ACE_ERR_CORRUPT = -3,
    ACE_ERR_TRUNCATED = -4,
    ACE_ERR_CRC = -5,
    ACE_ERR_PASSWORD = -6,
    ACE_ERR_METHOD = -7,
    ACE_ERR_NOT_ACE = -8,
    ACE_ERR_MULTIVOL = -9,
    ACE_ERR_PARAM = -10,
    ACE_ERR_EOF = -11
} ace_err_t;

const char *ace_strerror(int err);

static inline uint16_t ace_r16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t ace_r32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t ace_r64(const uint8_t *p)
{
    return (uint64_t)ace_r32(p) | ((uint64_t)ace_r32(p + 4) << 32);
}

static inline int8_t ace_schar(int i)
{
    return (int8_t)i;
}

static inline uint8_t ace_uchar(int i)
{
    return (uint8_t)i;
}

static inline uint32_t ace_add32(uint32_t a, uint32_t b)
{
    return a + b;
}

static inline uint32_t ace_rot32(uint32_t i, int n)
{
    if (n < 0)
        n = 32 + n;
    return (i << n) | (i >> (32 - n));
}

static inline int ace_bit_length(unsigned int x)
{
    int n = 0;
    while (x) {
        n++;
        x >>= 1;
    }
    return n;
}

int ace_mkdir_p(const char *path);
int ace_is_safe_relpath(const char *path);
void ace_normalize_path_separators(char *path);
void ace_format_unix_path(const char *src, char *dst, size_t dst_sz);
int ace_sanitize_path(const uint8_t *raw, size_t raw_len, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif
