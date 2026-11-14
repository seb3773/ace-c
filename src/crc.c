#include "ace/crc.h"

#include <string.h>

static uint32_t ace_crc32_table[8][256];
static int ace_crc32_ready;

static void ace_crc32_make_table(void)
{
    uint32_t i, j, c;
    for (i = 0; i < 256; i++) {
        c = i;
        for (j = 0; j < 8; j++) {
            if (c & 1)
                c = 0xEDB88320u ^ (c >> 1);
            else
                c >>= 1;
        }
        ace_crc32_table[0][i] = c;
    }
    for (i = 0; i < 256; i++) {
        c = ace_crc32_table[0][i];
        for (j = 1; j < 8; j++) {
            c = ace_crc32_table[0][c & 0xFF] ^ (c >> 8);
            ace_crc32_table[j][i] = c;
        }
    }
    ace_crc32_ready = 1;
}

void ace_crc32_init(ace_crc32_t *c)
{
    if (!ace_crc32_ready)
        ace_crc32_make_table();
    c->state = 0xFFFFFFFFu;
}

void ace_crc32_update(ace_crc32_t *c, const void *buf, size_t n)
{
    const uint8_t *p = (const uint8_t *)buf;
    uint32_t s = c->state;

    if (!ace_crc32_ready)
        ace_crc32_make_table();

    while (n >= 8) {
        uint32_t a = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                     ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        uint32_t b = (uint32_t)p[4] | ((uint32_t)p[5] << 8) |
                     ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
        s ^= a;
        s = ace_crc32_table[7][s & 0xFF] ^
            ace_crc32_table[6][(s >> 8) & 0xFF] ^
            ace_crc32_table[5][(s >> 16) & 0xFF] ^
            ace_crc32_table[4][s >> 24] ^
            ace_crc32_table[3][b & 0xFF] ^
            ace_crc32_table[2][(b >> 8) & 0xFF] ^
            ace_crc32_table[1][(b >> 16) & 0xFF] ^
            ace_crc32_table[0][b >> 24];
        p += 8;
        n -= 8;
    }
    while (n--) {
        s = ace_crc32_table[0][(s ^ *p++) & 0xFF] ^ (s >> 8);
    }
    c->state = s;
}

uint32_t ace_crc32_final(const ace_crc32_t *c)
{
    return c->state;
}

uint32_t ace_crc32(const void *buf, size_t n)
{
    ace_crc32_t c;
    ace_crc32_init(&c);
    ace_crc32_update(&c, buf, n);
    return ace_crc32_final(&c);
}

uint16_t ace_crc16(const void *buf, size_t n)
{
    return (uint16_t)(ace_crc32(buf, n) & 0xFFFF);
}
