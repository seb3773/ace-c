#include "ace/crc.h"

static uint32_t ace_crc32_table[256];
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
        ace_crc32_table[i] = c;
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
    size_t i;

    if (!ace_crc32_ready)
        ace_crc32_make_table();
    for (i = 0; i < n; i++)
        s = ace_crc32_table[(s ^ p[i]) & 0xFF] ^ (s >> 8);
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
