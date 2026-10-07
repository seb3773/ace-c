#include "ace/blowfish.h"

#include <string.h>

extern const uint32_t ace_bf_p[18];
extern const uint32_t ace_bf_s0[256];
extern const uint32_t ace_bf_s1[256];
extern const uint32_t ace_bf_s2[256];
extern const uint32_t ace_bf_s3[256];

static void sha1_derive(const uint8_t *pwd, size_t n, uint32_t key[5])
{
    uint32_t state[80];
    uint8_t buf[64];
    unsigned i;
    uint32_t a, b, c, d, e;

    if (n > 50)
        n = 50;
    memset(buf, 0, sizeof(buf));
    memcpy(buf, pwd, n);
    buf[n] = 0x80;
    for (i = 0; i < 15; i++)
        state[i] = ace_r32(buf + i * 4);
    state[15] = (uint32_t)(n << 3);
    for (i = 16; i < 80; i++)
        state[i] = state[i - 16] ^ state[i - 14] ^ state[i - 8] ^ state[i - 3];

    a = 0x67452301u;
    b = 0xefcdab89u;
    c = 0x98badcfeu;
    d = 0x10325476u;
    e = 0xc3d2e1f0u;
    for (i = 0; i < 20; i++) {
        uint32_t t = ace_rot32(a, 5) + ((b & c) | (~b & d)) + e + state[i] + 0x5a827999u;
        e = d;
        d = c;
        c = ace_rot32(b, 30);
        b = a;
        a = t;
    }
    for (i = 20; i < 40; i++) {
        uint32_t t = ace_rot32(a, 5) + (b ^ c ^ d) + e + state[i] + 0x6ed9eba1u;
        e = d;
        d = c;
        c = ace_rot32(b, 30);
        b = a;
        a = t;
    }
    for (i = 40; i < 60; i++) {
        uint32_t t = ace_rot32(a, 5) + ((b & c) | (b & d) | (c & d)) + e + state[i] + 0x8f1bbcdcu;
        e = d;
        d = c;
        c = ace_rot32(b, 30);
        b = a;
        a = t;
    }
    for (i = 60; i < 80; i++) {
        uint32_t t = ace_rot32(a, 5) + (b ^ c ^ d) + e + state[i] + 0xca62c1d6u;
        e = d;
        d = c;
        c = ace_rot32(b, 30);
        b = a;
        a = t;
    }
    key[0] = a + 0x67452301u;
    key[1] = b + 0xefcdab89u;
    key[2] = c + 0x98badcfeu;
    key[3] = d + 0x10325476u;
    key[4] = e + 0xc3d2e1f0u;
}

static uint32_t bf_func(ace_bf_t *bf, uint32_t x)
{
    uint32_t h = bf->s[0][x >> 24] + bf->s[1][(x >> 16) & 0xff];
    return (h ^ bf->s[2][(x >> 8) & 0xff]) + bf->s[3][x & 0xff];
}

static void bf_encrypt_block(ace_bf_t *bf, uint32_t *l, uint32_t *r)
{
    int i;
    uint32_t L = *l, R = *r;
    for (i = 0; i < 16; i += 2) {
        L ^= bf->p[i];
        R ^= bf_func(bf, L);
        R ^= bf->p[i + 1];
        L ^= bf_func(bf, R);
    }
    L ^= bf->p[16];
    R ^= bf->p[17];
    *l = R;
    *r = L;
}

static void bf_decrypt_block(ace_bf_t *bf, uint32_t *l, uint32_t *r)
{
    int i;
    uint32_t L = *l, R = *r;
    for (i = 16; i > 0; i -= 2) {
        L ^= bf->p[i + 1];
        R ^= bf_func(bf, L);
        R ^= bf->p[i];
        L ^= bf_func(bf, R);
    }
    L ^= bf->p[1];
    R ^= bf->p[0];
    *l = R;
    *r = L;
}

int ace_bf_init(ace_bf_t *bf, const uint8_t *pwd, size_t pwd_len)
{
    uint32_t key[5];
    uint32_t l = 0, r = 0;
    int i, j;

    sha1_derive(pwd, pwd_len, key);
    for (i = 0; i < 18; i++)
        bf->p[i] = ace_bf_p[i] ^ key[i % 5];
    memcpy(bf->s[0], ace_bf_s0, sizeof(ace_bf_s0));
    memcpy(bf->s[1], ace_bf_s1, sizeof(ace_bf_s1));
    memcpy(bf->s[2], ace_bf_s2, sizeof(ace_bf_s2));
    memcpy(bf->s[3], ace_bf_s3, sizeof(ace_bf_s3));
    bf->lastcl = 0;
    bf->lastcr = 0;
    for (i = 0; i < 18; i += 2) {
        bf_encrypt_block(bf, &l, &r);
        bf->p[i] = l;
        bf->p[i + 1] = r;
    }
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 256; j += 2) {
            bf_encrypt_block(bf, &l, &r);
            bf->s[i][j] = l;
            bf->s[i][j + 1] = r;
        }
    }
    return ACE_OK;
}

int ace_bf_decrypt(ace_bf_t *bf, uint8_t *buf, size_t n)
{
    size_t i;
    if (n % 8)
        return ACE_ERR_CORRUPT;
    for (i = 0; i < n; i += 8) {
        uint32_t cl = ace_r32(buf + i);
        uint32_t cr = ace_r32(buf + i + 4);
        uint32_t pl = cl, pr = cr;
        bf_decrypt_block(bf, &pl, &pr);
        pl ^= bf->lastcl;
        pr ^= bf->lastcr;
        bf->lastcl = cl;
        bf->lastcr = cr;
        buf[i] = (uint8_t)(pl);
        buf[i + 1] = (uint8_t)(pl >> 8);
        buf[i + 2] = (uint8_t)(pl >> 16);
        buf[i + 3] = (uint8_t)(pl >> 24);
        buf[i + 4] = (uint8_t)(pr);
        buf[i + 5] = (uint8_t)(pr >> 8);
        buf[i + 6] = (uint8_t)(pr >> 16);
        buf[i + 7] = (uint8_t)(pr >> 24);
    }
    return ACE_OK;
}

int ace_bf_encrypt(ace_bf_t *bf, uint8_t *buf, size_t n)
{
    size_t i;
    if (n % 8)
        return ACE_ERR_CORRUPT;
    for (i = 0; i < n; i += 8) {
        uint32_t pl = ace_r32(buf + i);
        uint32_t pr = ace_r32(buf + i + 4);
        pl ^= bf->lastcl;
        pr ^= bf->lastcr;
        bf_encrypt_block(bf, &pl, &pr);
        bf->lastcl = pl;
        bf->lastcr = pr;
        buf[i] = (uint8_t)(pl);
        buf[i + 1] = (uint8_t)(pl >> 8);
        buf[i + 2] = (uint8_t)(pl >> 16);
        buf[i + 3] = (uint8_t)(pl >> 24);
        buf[i + 4] = (uint8_t)(pr);
        buf[i + 5] = (uint8_t)(pr >> 8);
        buf[i + 6] = (uint8_t)(pr >> 16);
        buf[i + 7] = (uint8_t)(pr >> 24);
    }
    return ACE_OK;
}
