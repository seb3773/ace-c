#include "ace/archive.h"
#include "ace/crc.h"
#include "ace/bitstream.h"
#include "ace/blowfish.h"
#include "ace/compress.h"
#include "ace/engine.h"
#include "ace/huffman.h"
#include "ace/util.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails;

static void expect_u32(const char *name, uint32_t got, uint32_t want)
{
    if (got != want) {
        fprintf(stderr, "FAIL %s: got %u want %u\n", name, got, want);
        fails++;
    }
}

static void test_crc(void)
{
    expect_u32("crc32 123456789", ace_crc32("123456789", 9), 873187033u);
    expect_u32("crc16 123456789", ace_crc16("123456789", 9), 50905u);
}

static void test_bitstream(void)
{
    ace_bs_t bs;
    ace_memsrc_t src;
    const char *data = "01234567";
    uint32_t v;
    int gr;
    int rc = ace_bs_from_mem(&bs, &src, data, 8, 64);
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL bs init\n");
        fails++;
        return;
    }
    v = ace_bs_peek_bits(&bs, 31);
    expect_u32("peek 31", v, 429463704u);
    v = ace_bs_read_bits(&bs, 31);
    expect_u32("read 31", v, 429463704u);
    ace_bs_skip_bits(&bs, 3);
    v = ace_bs_read_bits(&bs, 5);
    expect_u32("read 5", v, 27u);
    ace_bs_read_golomb_rice(&bs, 3, 0, &gr);
    expect_u32("golomb 3", (uint32_t)gr, 20u);
    ace_bs_read_golomb_rice(&bs, 2, 1, &gr);
    if (gr != -2) {
        fprintf(stderr, "FAIL golomb signed got %d\n", gr);
        fails++;
    }
    v = ace_bs_read_knownwidth_uint(&bs, 10);
    expect_u32("knownwidth 10", v, 618u);
    v = ace_bs_read_bits(&bs, 7);
    expect_u32("read 7", v, 52u);
    ace_bs_free(&bs);
}

static void test_blowfish(void)
{
    ace_bf_t bf;
    uint8_t blk[8];
    memset(blk, 0xFF, 8);
    ace_bf_init(&bf, (const uint8_t *)"123456789", 9);
    ace_bf_decrypt(&bf, blk, 8);
    if (memcmp(blk, "\xb7wF@5.er", 8) != 0) {
        fprintf(stderr, "FAIL blowfish block1\n");
        fails++;
    }
    memset(blk, 0xC7, 8);
    ace_bf_decrypt(&bf, blk, 8);
    if (memcmp(blk, "eE\x05\xc4\xa5\x85)\xbc", 8) != 0) {
        fprintf(stderr, "FAIL blowfish block2\n");
        fails++;
    }
}

static void test_bsw(void)
{
    ace_bsw_t w;
    ace_bs_t bs;
    ace_memsrc_t src;
    uint32_t v;
    int rc;

    rc = ace_bsw_init(&w);
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL bsw init\n");
        fails++;
        return;
    }
    ace_bsw_write_bits(&w, 0x15A, 9);
    ace_bsw_write_bits(&w, 0, 4);
    ace_bsw_write_bits(&w, 12, 4);
    ace_bsw_pad32(&w);
    rc = ace_bs_from_mem(&bs, &src, w.data, w.len, 64);
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL bsw mem\n");
        fails++;
        ace_bsw_free(&w);
        return;
    }
    v = ace_bs_read_bits(&bs, 9);
    expect_u32("bsw 9", v, 0x15Au);
    v = ace_bs_read_bits(&bs, 4);
    expect_u32("bsw 4a", v, 0);
    v = ace_bs_read_bits(&bs, 4);
    expect_u32("bsw 4b", v, 12u);
    ace_bs_free(&bs);
    ace_bsw_free(&w);
}

typedef struct {
    uint8_t *buf;
    size_t n;
    size_t cap;
} ace_bufsink_t;

static int buf_out(void *ctx, const uint8_t *buf, size_t n)
{
    ace_bufsink_t *s = (ace_bufsink_t *)ctx;
    if (s->n + n > s->cap)
        return ACE_ERR_NOMEM;
    memcpy(s->buf + s->n, buf, n);
    s->n += n;
    return ACE_OK;
}

static void test_lz77_roundtrip(void)
{
    static const char *samples[] = {
        "Hello ACE 2.6 stored member\n",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        "the cat sat on the mat the cat sat on the mat",
        "0123456789abcdefghijklmnopqrstuvwxyz0123456789"
    };
    size_t s;
    for (s = 0; s < sizeof(samples) / sizeof(samples[0]); s++) {
        const uint8_t *in = (const uint8_t *)samples[s];
        size_t n = strlen(samples[s]);
        uint8_t *comp = NULL;
        size_t cn = 0;
        ace_engine_t e;
        ace_bs_t bs;
        ace_memsrc_t src;
        ace_bufsink_t sink;
        uint8_t out[1024];
        int rc = ace_compress_lz77(in, n, 65536, ACE_QUAL_NORMAL, &comp, &cn);
        if (rc != ACE_OK) {
            fprintf(stderr, "FAIL lz77 compress sample %zu rc=%d\n", s, rc);
            fails++;
            continue;
        }
        memset(&e, 0, sizeof(e));
        rc = ace_engine_init(&e);
        if (rc != ACE_OK) {
            fprintf(stderr, "FAIL engine init\n");
            fails++;
            free(comp);
            continue;
        }
        sink.buf = out;
        sink.n = 0;
        sink.cap = sizeof(out);
        rc = ace_bs_from_mem(&bs, &src, comp, cn, 4096);
        if (rc == ACE_OK)
            rc = ace_decompress_lz77(&e, &bs, n, 65536, buf_out, &sink);
        ace_bs_free(&bs);
        ace_engine_free(&e);
        if (rc != ACE_OK || sink.n != n || memcmp(out, in, n) != 0) {
            fprintf(stderr, "FAIL lz77 roundtrip sample %zu rc=%d n=%zu got=%zu\n",
                    s, rc, n, sink.n);
            fails++;
        }
        free(comp);
    }
}

static void test_blocked_roundtrip(void)
{
    uint8_t samples[3][512];
    size_t sizes[3];
    size_t s;

    memcpy(samples[0], "Hello ACE 2.0 blocked member\n", 29);
    sizes[0] = 29;
    memset(samples[1], 0, 512);
    {
        size_t i;
        for (i = 0; i < 256; i++) {
            samples[1][i * 2] = (uint8_t)(i & 0xFF);
            samples[1][i * 2 + 1] = 0x10;
        }
    }
    sizes[1] = 512;
    memset(samples[2], 0x90, 256);
    samples[2][0] = 'M';
    samples[2][1] = 'Z';
    samples[2][16] = 0xE8;
    samples[2][17] = 0x10;
    samples[2][18] = 0x00;
    samples[2][19] = 0x00;
    samples[2][20] = 0x00;
    samples[2][40] = 0xE9;
    samples[2][41] = 0x20;
    samples[2][42] = 0x00;
    sizes[2] = 256;

    for (s = 0; s < 3; s++) {
        uint8_t *comp = NULL;
        size_t cn = 0;
        ace_engine_t e;
        ace_bs_t bs;
        ace_memsrc_t src;
        ace_bufsink_t sink;
        uint8_t out[1024];
        int rc = ace_compress_blocked(samples[s], sizes[s], 65536, ACE_QUAL_NORMAL, &comp, &cn);
        if (rc != ACE_OK) {
            fprintf(stderr, "FAIL blocked compress sample %zu rc=%d\n", s, rc);
            fails++;
            continue;
        }
        memset(&e, 0, sizeof(e));
        rc = ace_engine_init(&e);
        if (rc != ACE_OK) {
            fprintf(stderr, "FAIL blocked engine init\n");
            fails++;
            free(comp);
            continue;
        }
        sink.buf = out;
        sink.n = 0;
        sink.cap = sizeof(out);
        rc = ace_bs_from_mem(&bs, &src, comp, cn, 4096);
        if (rc == ACE_OK)
            rc = ace_decompress_blocked(&e, &bs, sizes[s], 65536, buf_out, &sink);
        ace_bs_free(&bs);
        ace_engine_free(&e);
        if (rc != ACE_OK || sink.n != sizes[s] || memcmp(out, samples[s], sizes[s]) != 0) {
            fprintf(stderr, "FAIL blocked roundtrip sample %zu rc=%d n=%zu got=%zu\n",
                    s, rc, sizes[s], sink.n);
            fails++;
        }
        free(comp);
    }
}

static void test_golomb_roundtrip(void)
{
    int vals[] = {0, 1, -1, 2, -2, 20, -20, 127, -128};
    unsigned r;
    size_t i;
    for (r = 0; r <= 4; r++) {
        ace_bsw_t w;
        ace_bs_t bs;
        ace_memsrc_t src;
        int rc = ace_bsw_init(&w);
        if (rc != ACE_OK) {
            fails++;
            return;
        }
        for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++)
            ace_bsw_write_golomb_rice(&w, r, 1, vals[i]);
        ace_bsw_write_golomb_rice(&w, r, 0, 20);
        ace_bsw_pad32(&w);
        rc = ace_bs_from_mem(&bs, &src, w.data, w.len, 64);
        if (rc != ACE_OK) {
            fails++;
            ace_bsw_free(&w);
            return;
        }
        for (i = 0; i < sizeof(vals) / sizeof(vals[0]); i++) {
            int got = 0;
            ace_bs_read_golomb_rice(&bs, r, 1, &got);
            if (got != vals[i]) {
                fprintf(stderr, "FAIL golomb r=%u i=%zu got=%d want=%d\n", r, i, got, vals[i]);
                fails++;
            }
        }
        {
            int got = 0;
            ace_bs_read_golomb_rice(&bs, r, 0, &got);
            if (got != 20) {
                fprintf(stderr, "FAIL golomb unsigned r=%u got=%d\n", r, got);
                fails++;
            }
        }
        ace_bs_free(&bs);
        ace_bsw_free(&w);
    }
}

static int blocked_force_roundtrip(const uint8_t *in, size_t n, unsigned mode,
                                   int pic_w, int pic_p, const char *name)
{
    uint8_t *comp = NULL;
    size_t cn = 0;
    ace_engine_t e;
    ace_bs_t bs;
    ace_memsrc_t src;
    ace_bufsink_t sink;
    uint8_t *out;
    int rc;

    out = (uint8_t *)malloc(n ? n : 1);
    if (!out)
        return -1;
    rc = ace_compress_blocked_ex(in, n, 65536, ACE_QUAL_NORMAL, mode, pic_w, pic_p, &comp, &cn);
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL %s compress rc=%d\n", name, rc);
        free(out);
        return -1;
    }
    memset(&e, 0, sizeof(e));
    rc = ace_engine_init(&e);
    sink.buf = out;
    sink.n = 0;
    sink.cap = n;
    if (rc == ACE_OK)
        rc = ace_bs_from_mem(&bs, &src, comp, cn, 4096);
    if (rc == ACE_OK)
        rc = ace_decompress_blocked(&e, &bs, n, 65536, buf_out, &sink);
    ace_bs_free(&bs);
    ace_engine_free(&e);
    if (rc != ACE_OK || sink.n != n || memcmp(out, in, n) != 0) {
        fprintf(stderr, "FAIL %s roundtrip rc=%d n=%zu got=%zu cn=%zu\n", name, rc, n, sink.n, cn);
        free(comp);
        free(out);
        return -1;
    }
    free(comp);
    free(out);
    return 0;
}

static void test_sound_pic_roundtrip(void)
{
    uint8_t sound[256];
    uint8_t pic[64 * 8];
    size_t i;
    for (i = 0; i < sizeof(sound); i++)
        sound[i] = (uint8_t)(128 + ((i / 4) & 15) - 8);
    for (i = 0; i < sizeof(pic); i++) {
        unsigned x = (unsigned)(i % 64);
        unsigned y = (unsigned)(i / 64);
        pic[i] = (uint8_t)(x + y * 3);
    }
    if (blocked_force_roundtrip(sound, sizeof(sound), ACE_MODE_SOUND_8, 0, 0, "sound8"))
        fails++;
    if (blocked_force_roundtrip(sound, sizeof(sound), ACE_MODE_SOUND_16, 0, 0, "sound16"))
        fails++;
    if (blocked_force_roundtrip(pic, sizeof(pic), ACE_MODE_PIC, 64, 1, "pic"))
        fails++;
    {
        uint8_t rgb[48 * 4];
        for (i = 0; i < sizeof(rgb); i++) {
            unsigned x = (unsigned)(i % 48);
            unsigned y = (unsigned)(i / 48);
            unsigned p = x % 3;
            rgb[i] = (uint8_t)(40 * p + x + y);
        }
        if (blocked_force_roundtrip(rgb, sizeof(rgb), ACE_MODE_PIC, 48, 3, "pic3"))
            fails++;
    }
    if (blocked_force_roundtrip(sound, sizeof(sound), ACE_MODE_SOUND_32A, 0, 0, "sound32a"))
        fails++;
    if (blocked_force_roundtrip(sound, sizeof(sound), ACE_MODE_SOUND_32B, 0, 0, "sound32b"))
        fails++;
    {
        uint8_t pic_left[64 * 3 + 17];
        for (i = 0; i < sizeof(pic_left); i++)
            pic_left[i] = (uint8_t)(i * 7);
        if (blocked_force_roundtrip(pic_left, sizeof(pic_left), ACE_MODE_PIC, 64, 1, "pic-leftover"))
            fails++;
    }
}

static void test_huffman_from_freq(void)
{
    uint32_t freq[16];
    ace_huff_t t;
    ace_bsw_t w;
    ace_bs_t bs;
    ace_memsrc_t src;
    ace_huff_t rt;
    unsigned i;
    int rc;

    memset(freq, 0, sizeof(freq));
    freq[0] = 50;
    freq[1] = 20;
    freq[2] = 10;
    freq[5] = 5;
    freq[7] = 1;
    memset(&t, 0, sizeof(t));
    rc = ace_huff_from_freq(freq, 16, 11, &t);
    if (rc != ACE_OK || !t.widths) {
        fprintf(stderr, "FAIL huff from_freq rc=%d\n", rc);
        fails++;
        return;
    }
    for (i = 0; i < t.nwidths; i++) {
        if (freq[i] && t.widths[i] == 0) {
            fprintf(stderr, "FAIL huff width 0 for used sym %u\n", i);
            fails++;
        }
        if (t.widths[i] > 11) {
            fprintf(stderr, "FAIL huff width %u > 11\n", t.widths[i]);
            fails++;
        }
    }
    rc = ace_bsw_init(&w);
    if (rc == ACE_OK)
        rc = ace_huff_write_tree(&w, &t);
    for (i = 0; rc == ACE_OK && i < 8; i++) {
        static const unsigned seq[] = {0, 1, 2, 0, 5, 0, 1, 7};
        rc = ace_huff_write_symbol(&w, &t, seq[i]);
    }
    if (rc == ACE_OK)
        rc = ace_bsw_pad32(&w);
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL huff write rc=%d\n", rc);
        fails++;
        ace_huff_free(&t);
        ace_bsw_free(&w);
        return;
    }
    memset(&rt, 0, sizeof(rt));
    rc = ace_bs_from_mem(&bs, &src, w.data, w.len, 64);
    if (rc == ACE_OK)
        rc = ace_huff_read_tree(&bs, 11, 16, &rt);
    for (i = 0; rc == ACE_OK && i < 8; i++) {
        static const unsigned seq[] = {0, 1, 2, 0, 5, 0, 1, 7};
        unsigned got = 0;
        rc = ace_huff_read_symbol(&rt, &bs, &got);
        if (rc == ACE_OK && got != seq[i]) {
            fprintf(stderr, "FAIL huff roundtrip i=%u got=%u want=%u\n", i, got, seq[i]);
            fails++;
        }
    }
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL huff read rc=%d\n", rc);
        fails++;
    }
    ace_huff_free(&rt);
    ace_huff_free(&t);
    ace_bs_free(&bs);
    ace_bsw_free(&w);
}

static void test_solid_hist(void)
{
    const char *a = "the cat sat on the mat the cat sat on the mat\n";
    const char *b = "the cat sat on the mat again and again and again\n";
    ace_comp_hist_t hist;
    uint8_t *c1 = NULL, *c2 = NULL, *c2ns = NULL;
    size_t n1 = 0, n2 = 0, n2ns = 0;
    ace_engine_t e;
    ace_bs_t bs;
    ace_memsrc_t src;
    ace_bufsink_t sink;
    uint8_t out[256];
    int rc;

    ace_comp_hist_init(&hist);
    rc = ace_compress_lz77_hist((const uint8_t *)a, strlen(a), 65536, 3, &hist, &c1, &n1);
    if (rc == ACE_OK)
        rc = ace_compress_lz77_hist((const uint8_t *)b, strlen(b), 65536, 3, &hist, &c2, &n2);
    if (rc != ACE_OK) {
        fprintf(stderr, "FAIL solid hist compress rc=%d\n", rc);
        fails++;
        ace_comp_hist_free(&hist);
        free(c1);
        free(c2);
        return;
    }
    ace_compress_lz77((const uint8_t *)b, strlen(b), 65536, 3, &c2ns, &n2ns);
    memset(&e, 0, sizeof(e));
    ace_engine_init(&e);
    sink.buf = out;
    sink.n = 0;
    sink.cap = sizeof(out);
    ace_bs_from_mem(&bs, &src, c1, n1, 4096);
    rc = ace_decompress_lz77(&e, &bs, strlen(a), 65536, buf_out, &sink);
    ace_bs_free(&bs);
    if (rc != ACE_OK || sink.n != strlen(a) || memcmp(out, a, strlen(a)) != 0) {
        fprintf(stderr, "FAIL solid first member\n");
        fails++;
    }
    sink.n = 0;
    ace_bs_from_mem(&bs, &src, c2, n2, 4096);
    rc = ace_decompress_lz77(&e, &bs, strlen(b), 65536, buf_out, &sink);
    ace_bs_free(&bs);
    ace_engine_free(&e);
    if (rc != ACE_OK || sink.n != strlen(b) || memcmp(out, b, strlen(b)) != 0) {
        fprintf(stderr, "FAIL solid second member rc=%d n=%zu\n", rc, sink.n);
        fails++;
    }
    ace_comp_hist_free(&hist);
    free(c1);
    free(c2);
    free(c2ns);
}

static void test_midstream_switch(void)
{
    uint8_t buf[400];
    size_t i;
    memcpy(buf, "Hello ACE mixed stream prefix................", 45);
    for (i = 45; i < 45 + 256; i++)
        buf[i] = (uint8_t)(128 + ((i / 4) & 15) - 8);
    for (i = 45 + 256; i < sizeof(buf); i++)
        buf[i] = (uint8_t)(i * 9);
    if (blocked_force_roundtrip(buf, sizeof(buf), 0, 0, 0, "midstream-auto"))
        fails++;
}

static void test_quality_levels(void)
{
    uint8_t buf[4096];
    size_t i;
    unsigned q;
    for (i = 0; i < sizeof(buf); i++)
        buf[i] = (uint8_t)((i * 31) ^ (i >> 3));
    memcpy(buf, "the cat sat on the mat the cat sat on the mat ", 46);
    for (q = 1; q <= 5; q++) {
        uint8_t *comp = NULL;
        size_t cn = 0;
        ace_engine_t e;
        ace_bs_t bs;
        ace_memsrc_t src;
        ace_bufsink_t sink;
        uint8_t out[4096];
        int rc = ace_compress_lz77(buf, sizeof(buf), 32768, q, &comp, &cn);
        if (rc != ACE_OK) {
            fprintf(stderr, "FAIL quality %u compress rc=%d\n", q, rc);
            fails++;
            continue;
        }
        memset(&e, 0, sizeof(e));
        rc = ace_engine_init(&e);
        sink.buf = out;
        sink.n = 0;
        sink.cap = sizeof(out);
        if (rc == ACE_OK)
            rc = ace_bs_from_mem(&bs, &src, comp, cn, 4096);
        if (rc == ACE_OK)
            rc = ace_decompress_lz77(&e, &bs, sizeof(buf), 32768, buf_out, &sink);
        ace_bs_free(&bs);
        ace_engine_free(&e);
        if (rc != ACE_OK || sink.n != sizeof(buf) || memcmp(out, buf, sizeof(buf)) != 0) {
            fprintf(stderr, "FAIL quality %u roundtrip rc=%d n=%zu\n", q, rc, sink.n);
            fails++;
        }
        free(comp);
    }
}

static void test_auto_blocked_not_sound_pic(void)
{
    uint8_t hello[] = "Hello ACE 2.0 blocked member\n";
    uint8_t exe[256];
    uint8_t delta[512];
    size_t i;

    memset(exe, 0x90, sizeof(exe));
    exe[0] = 'M';
    exe[1] = 'Z';
    exe[16] = 0xE8;
    exe[17] = 0x10;
    exe[40] = 0xE9;
    exe[41] = 0x20;
    memset(delta, 0, sizeof(delta));
    for (i = 0; i < 256; i++) {
        delta[i * 2] = (uint8_t)i;
        delta[i * 2 + 1] = 0x10;
    }
    if (blocked_force_roundtrip(hello, sizeof(hello) - 1, 0, 0, 0, "auto-hello"))
        fails++;
    if (blocked_force_roundtrip(exe, sizeof(exe), 0, 0, 0, "auto-exe"))
        fails++;
    if (blocked_force_roundtrip(delta, sizeof(delta), 0, 0, 0, "auto-delta"))
        fails++;
}

static void test_path_safety(void)
{
    /* Safe paths */
    expect_u32("safe file", ace_is_safe_relpath("file.txt"), 1);
    expect_u32("safe subfile", ace_is_safe_relpath("sub/file.txt"), 1);
    expect_u32("safe dos backslash", ace_is_safe_relpath("DIR\\SUBDIR\\FILE.TXT"), 1);
    expect_u32("safe dot slash", ace_is_safe_relpath("./file.txt"), 1);
    expect_u32("safe mid dot", ace_is_safe_relpath("sub/./file.txt"), 1);
    expect_u32("safe non-ascending dotdot", ace_is_safe_relpath("a/b/../c/file.txt"), 1);

    /* Unsafe traversal */
    expect_u32("unsafe dotdot start", ace_is_safe_relpath("../file.txt"), 0);
    expect_u32("unsafe dos dotdot start", ace_is_safe_relpath("..\\file.txt"), 0);
    expect_u32("unsafe ascending dotdot", ace_is_safe_relpath("a/../../file.txt"), 0);
    expect_u32("unsafe dos ascending dotdot", ace_is_safe_relpath("a\\..\\..\\file.txt"), 0);
    expect_u32("unsafe root dot", ace_is_safe_relpath("."), 0);
    expect_u32("unsafe root dotdot", ace_is_safe_relpath(".."), 0);
    expect_u32("unsafe root resolve", ace_is_safe_relpath("a/.."), 0);

    /* Unsafe absolute and drive */
    expect_u32("unsafe posix root", ace_is_safe_relpath("/etc/passwd"), 0);
    expect_u32("unsafe dos root", ace_is_safe_relpath("\\windows\\win.ini"), 0);
    expect_u32("unsafe dos drive C:", ace_is_safe_relpath("C:\\autoexec.bat"), 0);
    expect_u32("unsafe dos drive c: lower", ace_is_safe_relpath("c:/boot.ini"), 0);
    expect_u32("unsafe dos drive no slash", ace_is_safe_relpath("D:file.txt"), 0);
    expect_u32("unsafe embedded drive", ace_is_safe_relpath("sub/C:/file.txt"), 0);
    expect_u32("unsafe empty", ace_is_safe_relpath(""), 0);
    expect_u32("unsafe null", ace_is_safe_relpath(NULL), 0);

    /* Path separator normalization */
    char norm_test[64] = "DIR\\SUBDIR\\FILE.TXT";
    ace_normalize_path_separators(norm_test);
    if (strcmp(norm_test, "DIR/SUBDIR/FILE.TXT") != 0) {
        fprintf(stderr, "FAIL normalize_path_separators: got %s\n", norm_test);
        fails++;
    }

    /* Unix path formatting */
    char unix_test[64];
    ace_format_unix_path("DIR\\SUBDIR\\FILE.TXT", unix_test, sizeof(unix_test));
    if (strcmp(unix_test, "dir/subdir/file.txt") != 0) {
        fprintf(stderr, "FAIL ace_format_unix_path: got %s\n", unix_test);
        fails++;
    }
    ace_format_unix_path("README.TXT", unix_test, sizeof(unix_test));
    if (strcmp(unix_test, "readme.txt") != 0) {
        fprintf(stderr, "FAIL ace_format_unix_path simple: got %s\n", unix_test);
        fails++;
    }
}

int main(void)
{
    test_crc();
    test_bitstream();
    test_blowfish();
    test_bsw();
    test_golomb_roundtrip();
    test_lz77_roundtrip();
    test_blocked_roundtrip();
    test_sound_pic_roundtrip();
    test_auto_blocked_not_sound_pic();
    test_huffman_from_freq();
    test_quality_levels();
    test_solid_hist();
    test_midstream_switch();
    test_path_safety();
    if (fails) {
        fprintf(stderr, "%d failure(s)\n", fails);
        return 1;
    }
    printf("test_core: ok\n");
    return 0;
}
