#define _DEFAULT_SOURCE 1
#define _GNU_SOURCE 1
#include "ace/archive.h"
#include "ace/blowfish.h"
#include "ace/cli.h"
#include "ace/compress.h"
#include "ace/crc.h"
#include "ace/sfx.h"
#include "ace/util.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#ifndef strcasecmp
#define strcasecmp _stricmp
#endif
#ifndef strncasecmp
#define strncasecmp _strnicmp
#endif
#endif

/* Exclusion list structure and helpers for -x / -x@list */
typedef struct {
    char **patterns;
    size_t count;
    size_t cap;
} exclude_list_t;

static void exclude_add(exclude_list_t *el, const char *pat)
{
    if (el->count == el->cap) {
        size_t ncap = el->cap ? el->cap * 2 : 16;
        char **np = (char **)realloc(el->patterns, ncap * sizeof(*np));
        if (!np)
            return;
        el->patterns = np;
        el->cap = ncap;
    }
    el->patterns[el->count++] = strdup(pat);
}

static void exclude_free(exclude_list_t *el)
{
    size_t i;
    for (i = 0; i < el->count; i++)
        free(el->patterns[i]);
    free(el->patterns);
    el->patterns = NULL;
    el->count = el->cap = 0;
}

static int exclude_load_file(exclude_list_t *el, const char *path)
{
    FILE *fp = (strcmp(path, "-") == 0) ? stdin : fopen(path, "r");
    char line[4096];
    if (!fp) {
        perror(path);
        return ACE_ERR_IO;
    }
    while (fgets(line, sizeof(line), fp)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                           line[len - 1] == ' ' || line[len - 1] == '\t'))
            line[--len] = '\0';
        if (len > 0 && line[0] != '#')
            exclude_add(el, line);
    }
    if (fp != stdin)
        fclose(fp);
    return ACE_OK;
}

static int is_excluded(const exclude_list_t *el, const char *path, const char *stored)
{
    size_t i;
    const char *b_path, *b_stored;
    char norm_path[4096];
    size_t len, k;

    if (!el || el->count == 0)
        return 0;

    b_path = strrchr(path, '/');
    b_path = b_path ? b_path + 1 : path;

    b_stored = strrchr(stored, '\\');
    b_stored = b_stored ? b_stored + 1 : stored;

    len = strlen(path);
    if (len >= sizeof(norm_path))
        len = sizeof(norm_path) - 1;
    for (k = 0; k < len; k++)
        norm_path[k] = (path[k] == '\\') ? '/' : path[k];
    norm_path[len] = '\0';

    for (i = 0; i < el->count; i++) {
        const char *pat = el->patterns[i];
        size_t plen = strlen(pat);
        if (ace_wildcard_match(pat, path, 1) ||
            ace_wildcard_match(pat, norm_path, 1) ||
            ace_wildcard_match(pat, stored, 1) ||
            ace_wildcard_match(pat, b_path, 1) ||
            ace_wildcard_match(pat, b_stored, 1))
            return 1;
        if (plen > 0 && (pat[plen - 1] == '/' || pat[plen - 1] == '\\')) {
            if (strncasecmp(norm_path, pat, plen) == 0 ||
                strncasecmp(path, pat, plen) == 0)
                return 1;
        } else {
            if (strncasecmp(norm_path, pat, plen) == 0 && norm_path[plen] == '/')
                return 1;
            if (strncasecmp(b_path, pat, plen) == 0 && b_path[plen] == '/')
                return 1;
        }
    }
    return 0;
}

/* Encode a Unix time as an ACE/DOS timestamp. The DOS year field is 7 bits
 * wide (offset from 1980, range 1980..2107); out-of-range times are clamped to
 * the nearest representable year so we never emit a corrupt header. */
static uint32_t dos_from_time(time_t t)
{
    struct tm *tm = localtime(&t);
    long off;
    if (!tm)
        return (1u << 21) | (1u << 16); /* 1980-01-01 00:00:00 */
    off = (long)tm->tm_year - 80;
    if (off < 0)
        off = 0;
    else if (off > 127)
        off = 127;
    return ((uint32_t)off << 25) |
           ((uint32_t)(tm->tm_mon + 1) << 21) |
           ((uint32_t)tm->tm_mday << 16) |
           ((uint32_t)tm->tm_hour << 11) |
           ((uint32_t)tm->tm_min << 5) |
           ((uint32_t)(tm->tm_sec / 2));
}

static uint32_t dos_now(void)
{
    return dos_from_time(time(NULL));
}

/* Map a POSIX st_mode to the ACE/DOS 32-bit attribute field. */
static uint32_t dos_attrs(mode_t m)
{
    uint32_t a = 0;
    if (S_ISDIR(m))
        a |= ACE_ATTR_DIRECTORY;
    else
        a |= ACE_ATTR_ARCHIVE;
    if (!(m & S_IWUSR))
        a |= ACE_ATTR_READONLY;
    return a;
}

#ifdef _WIN32
static uint32_t dos_attrs_path(const char *path, mode_t m)
{
    DWORD dw = GetFileAttributesA(path);
    if (dw != INVALID_FILE_ATTRIBUTES) {
        uint32_t a = 0;
        if (dw & FILE_ATTRIBUTE_READONLY)  a |= ACE_ATTR_READONLY;
        if (dw & FILE_ATTRIBUTE_HIDDEN)    a |= ACE_ATTR_HIDDEN;
        if (dw & FILE_ATTRIBUTE_SYSTEM)    a |= ACE_ATTR_SYSTEM;
        if (dw & FILE_ATTRIBUTE_DIRECTORY) a |= ACE_ATTR_DIRECTORY;
        if (dw & FILE_ATTRIBUTE_ARCHIVE)   a |= ACE_ATTR_ARCHIVE;
        return a;
    }
    return dos_attrs(m);
}
#endif

static int write_header(FILE *fp, const uint8_t *payload, uint16_t n)
{
    uint16_t crc = ace_crc16(payload, n);
    uint8_t pre[4];
    pre[0] = (uint8_t)crc;
    pre[1] = (uint8_t)(crc >> 8);
    pre[2] = (uint8_t)n;
    pre[3] = (uint8_t)(n >> 8);
    if (fwrite(pre, 1, 4, fp) != 4)
        return ACE_ERR_IO;
    if (fwrite(payload, 1, n, fp) != n)
        return ACE_ERR_IO;
    return ACE_OK;
}

static void usage(const char *argv0)
{
    fprintf(stderr,
        "Usage: %s [-0|-z|-2|-s|-s8|-s16|-s32a|-s32b|-p W[:P]] [-xe|-dl] [-m 0-5] [-d KB]\n"
        "          [-pw PASS] [-cm TEXT] [-cf TEXT] [-V BYTES] [-A] [-k] [-x PAT] -o ARCHIVE FILE|@LIST...\n"
        "  -0       store uncompressed (default)\n"
        "  -z       LZ77 (ACE 1.0 method)\n"
        "  -2       blocked ACE 2.0 (auto LZ77/EXE/DELTA/SOUND/PIC)\n"
        "  -s       solid (shared LZ77 dictionary)\n"
        "  -s8      blocked SOUND_8\n"
        "  -s16     blocked SOUND_16\n"
        "  -s32a    blocked SOUND_32A\n"
        "  -s32b    blocked SOUND_32B\n"
        "  -xe      blocked, force EXE preprocessing\n"
        "  -dl      blocked, force DELTA preprocessing\n"
        "  -p W[:P] blocked PIC width W, planes P (default 1)\n"
        "  -sfx[=T] create self-extracting archive (T: dos, win32, gui, linux; default dos)\n"
        "  -m 0-5   compression quality (default 3)\n"
        "  -d KB    dictionary size in KiB: 32..4096 (default 64)\n"
        "  -pw PASS encrypt members with password PASS\n"
        "  -cm TEXT set main archive comment\n"
        "  -cf TEXT set comment for all files\n"
        "  -V BYTES split into volumes of at most BYTES packed bytes (.c00..)\n"
        "  -A       store full path (ACE backslash style) instead of basename\n"
        "  -k       lock archive (set FLAG_LOCKED in main header)\n"
        "  -x PAT   exclude files matching PAT (-x@LIST for exclusion file)\n"
        "  @LIST    read files to archive from text file (one per line, @- for stdin)\n"
        "  FILE...  regular files, or directories stored recursively\n",
        argv0);
}

/* Archive-wide parameters shared by every volume's main header. */
typedef struct {
    const char *outpath;
    uint16_t main_flags;
    uint32_t dt;
    const uint8_t *main_cm;
    size_t main_cm_n;
    int multivol;
    size_t volsize;
    int method;
    uint8_t host;
    ace_sfx_type_t sfx_type;
} volcfg_t;

/* Streaming volume writer: opens one FILE at a time, writes MAIN on demand. */
typedef struct {
    volcfg_t *cfg;
    FILE *fp;
    unsigned nextvol;
    int opened;
    size_t remain;
} volw_t;

static void vol_name(const char *outpath, unsigned vol, char *dst, size_t n)
{
    const char *dot;
    size_t sl;
    char stem[4096];

    if (vol == 0) {
        snprintf(dst, n, "%s", outpath);
        return;
    }
    dot = strrchr(outpath, '.');
    if (!dot || dot == outpath)
        sl = strlen(outpath);
    else
        sl = (size_t)(dot - outpath);
    if (sl >= sizeof(stem))
        sl = sizeof(stem) - 1;
    memcpy(stem, outpath, sl);
    stem[sl] = 0;
    snprintf(dst, n, "%.*s.c%02u", (int)(n > 16 ? n - 16 : 0), stem, vol - 1);
}

static const char ACE_ADVERT_UNREG[] = "*UNREGISTERED VERSION*";
#define ACE_ADVERT_UNREG_LEN 22

static int vol_write_main(const volw_t *w, unsigned vol)
{
    const volcfg_t *c = w->cfg;
    uint16_t mf = c->main_flags | ACE_FLAG_ADVERT;
    size_t cmn = (vol == 0) ? c->main_cm_n : 0;
    uint8_t *mb;
    size_t n = 0;
    int rc;
    uint8_t ev = (c->method == 1) ? 10 : 20;
    uint8_t host = c->host ? c->host : ACE_HOST_WIN32;
    uint16_t r_low = (uint16_t)(ace_crc16(ACE_ADVERT_UNREG, ACE_ADVERT_UNREG_LEN) ^ (c->dt & 0xffff));
    uint16_t r_high = (uint16_t)(((c->dt >> 12) & 0xffff) ^ r_low);

    if (c->multivol)
        mf |= ACE_FLAG_MULTIVOLUME;
    if (vol != 0)
        mf &= (uint16_t)~ACE_FLAG_COMMENT;
    mb = (uint8_t *)malloc(32 + ACE_ADVERT_UNREG_LEN + cmn + 2);
    if (!mb)
        return ACE_ERR_NOMEM;
    mb[n++] = (uint8_t)ACE_TYPE_MAIN;
    mb[n++] = (uint8_t)mf;
    mb[n++] = (uint8_t)(mf >> 8);
    memcpy(mb + n, ACE_MAGIC, 7);
    n += 7;
    mb[n++] = ev;
    mb[n++] = 20;
    mb[n++] = host;
    mb[n++] = (uint8_t)vol;
    mb[n++] = (uint8_t)c->dt;
    mb[n++] = (uint8_t)(c->dt >> 8);
    mb[n++] = (uint8_t)(c->dt >> 16);
    mb[n++] = (uint8_t)(c->dt >> 24);
    mb[n++] = (uint8_t)r_low;
    mb[n++] = (uint8_t)(r_low >> 8);
    mb[n++] = (uint8_t)r_high;
    mb[n++] = (uint8_t)(r_high >> 8);
    mb[n++] = 0;
    mb[n++] = 0;
    mb[n++] = 0;
    mb[n++] = 0;
    mb[n++] = ACE_ADVERT_UNREG_LEN;
    memcpy(mb + n, ACE_ADVERT_UNREG, ACE_ADVERT_UNREG_LEN);
    n += ACE_ADVERT_UNREG_LEN;
    if (cmn) {
        mb[n++] = (uint8_t)cmn;
        mb[n++] = (uint8_t)(cmn >> 8);
        memcpy(mb + n, c->main_cm, cmn);
        n += cmn;
    }
    rc = write_header(w->fp, mb, (uint16_t)n);
    free(mb);
    return rc;
}

static int vol_ensure(volw_t *w)
{
    char p[4096];
    int rc;

    if (w->opened)
        return ACE_OK;
    vol_name(w->cfg->outpath, w->nextvol, p, sizeof(p));
    w->fp = fopen(p, "wb");
    if (!w->fp) {
        perror(p);
        return ACE_ERR_IO;
    }
    if (w->nextvol == 0 && w->cfg->sfx_type != ACE_SFX_NONE) {
        size_t stub_sz = 0;
        const uint8_t *stub = ace_sfx_get_stub(w->cfg->sfx_type, &stub_sz);
        if (stub && stub_sz > 0) {
            if (fwrite(stub, 1, stub_sz, w->fp) != stub_sz) {
                fclose(w->fp);
                w->fp = NULL;
                return ACE_ERR_IO;
            }
        }
    }
    rc = vol_write_main(w, w->nextvol);
    if (rc != ACE_OK) {
        fclose(w->fp);
        w->fp = NULL;
        return rc;
    }
    w->opened = 1;
    w->remain = w->cfg->volsize;
    w->nextvol++;
    return ACE_OK;
}

static void vol_close(volw_t *w)
{
    if (w->opened) {
        fclose(w->fp);
        w->fp = NULL;
        w->opened = 0;
    }
    if (w->cfg && w->cfg->sfx_type != ACE_SFX_NONE && w->cfg->outpath) {
        chmod(w->cfg->outpath, 0755);
    }
}

static size_t build_file_body(uint8_t *b, uint16_t fflags, uint64_t packsz, uint64_t origsz,
                              uint32_t dt, uint32_t attr, uint32_t crc, uint8_t comptype,
                              uint8_t compqual, uint16_t params, const char *name, size_t namelen,
                              const uint8_t *cm, size_t cm_n)
{
    size_t n = 0;
    int is64 = (packsz > 0xFFFFFFFFULL || origsz > 0xFFFFFFFFULL || getenv("ACE_FORCE_FILE64") != NULL);

    if (is64) {
        b[n++] = (uint8_t)ACE_TYPE_FILE64;
        fflags |= ACE_FLAG_64BIT;
    } else {
        b[n++] = (uint8_t)ACE_TYPE_FILE32;
    }
    b[n++] = (uint8_t)fflags;
    b[n++] = (uint8_t)(fflags >> 8);
    if (is64) {
        b[n++] = (uint8_t)packsz;
        b[n++] = (uint8_t)(packsz >> 8);
        b[n++] = (uint8_t)(packsz >> 16);
        b[n++] = (uint8_t)(packsz >> 24);
        b[n++] = (uint8_t)(packsz >> 32);
        b[n++] = (uint8_t)(packsz >> 40);
        b[n++] = (uint8_t)(packsz >> 48);
        b[n++] = (uint8_t)(packsz >> 56);
        b[n++] = (uint8_t)origsz;
        b[n++] = (uint8_t)(origsz >> 8);
        b[n++] = (uint8_t)(origsz >> 16);
        b[n++] = (uint8_t)(origsz >> 24);
        b[n++] = (uint8_t)(origsz >> 32);
        b[n++] = (uint8_t)(origsz >> 40);
        b[n++] = (uint8_t)(origsz >> 48);
        b[n++] = (uint8_t)(origsz >> 56);
    } else {
        b[n++] = (uint8_t)packsz;
        b[n++] = (uint8_t)(packsz >> 8);
        b[n++] = (uint8_t)(packsz >> 16);
        b[n++] = (uint8_t)(packsz >> 24);
        b[n++] = (uint8_t)origsz;
        b[n++] = (uint8_t)(origsz >> 8);
        b[n++] = (uint8_t)(origsz >> 16);
        b[n++] = (uint8_t)(origsz >> 24);
    }
    b[n++] = (uint8_t)dt;
    b[n++] = (uint8_t)(dt >> 8);
    b[n++] = (uint8_t)(dt >> 16);
    b[n++] = (uint8_t)(dt >> 24);
    b[n++] = (uint8_t)attr;
    b[n++] = (uint8_t)(attr >> 8);
    b[n++] = (uint8_t)(attr >> 16);
    b[n++] = (uint8_t)(attr >> 24);
    b[n++] = (uint8_t)crc;
    b[n++] = (uint8_t)(crc >> 8);
    b[n++] = (uint8_t)(crc >> 16);
    b[n++] = (uint8_t)(crc >> 24);
    b[n++] = comptype;
    b[n++] = compqual;
    b[n++] = (uint8_t)params;
    b[n++] = (uint8_t)(params >> 8);
    b[n++] = 0x54;
    b[n++] = 0x45;
    b[n++] = (uint8_t)namelen;
    b[n++] = (uint8_t)(namelen >> 8);
    memcpy(b + n, name, namelen);
    n += namelen;
    if (cm_n) {
        b[n++] = (uint8_t)cm_n;
        b[n++] = (uint8_t)(cm_n >> 8);
        memcpy(b + n, cm, cm_n);
        n += cm_n;
    }
    return n;
}

/* Build the stored member name. ACE uses backslash separators for paths and
 * long names; with full==0 we keep only the basename. Absolute and "./"
 * prefixes are stripped so we never emit a rooted path. */
static size_t stored_name(const char *arg, int full, char *dst, size_t n)
{
    const char *src = arg;
    size_t i = 0;

    if (n == 0)
        return 0;
    if (!full) {
        const char *b = strrchr(arg, '/');
        if (b)
            src = b + 1;
    } else {
        while (src[0] == '.' && (src[1] == '/' || src[1] == 0))
            src += (src[1] == '/') ? 2 : 1;
        while (src[0] == '/')
            src++;
    }
    for (i = 0; i + 1 < n && src[i]; i++)
        dst[i] = (src[i] == '/') ? '\\' : src[i];
    dst[i] = 0;
    return i;
}

/* Per-run encode settings shared by the recursive directory walk. */
typedef struct {
    int method;
    unsigned quality;
    size_t dicsize;
    uint16_t params;
    unsigned force_mode;
    int pic_width;
    int pic_planes;
    int solid;
    int store_paths;
    const char *password;
    const uint8_t *file_cm;
    size_t file_cm_n;
    ace_comp_hist_t *hist;
    const exclude_list_t *excludes;
} job_t;

/* Write one FILE32 or FILE64 entry (header + payload), splitting the payload
 * across volumes when the archive is multivolume and the payload is non-empty. */
static int emit_entry(volw_t *w, uint16_t hflags, const uint8_t *payload, uint64_t packsize,
                      uint64_t origsz, uint32_t dt, uint32_t attr, uint32_t crc,
                      uint8_t comptype, uint8_t compqual, uint16_t params,
                      const char *name, size_t namelen, const uint8_t *cm, size_t cm_n)
{
    uint8_t *body = (uint8_t *)malloc(48 + namelen + cm_n + 2);
    size_t off = 0;
    int firstpart = 1;
    int rc = ACE_OK;

    if (!body)
        return ACE_ERR_NOMEM;
    if (!w->cfg->multivol || packsize == 0) {
        size_t blen;
        if (vol_ensure(w) != ACE_OK) {
            free(body);
            return ACE_ERR_IO;
        }
        blen = build_file_body(body, hflags, packsize, origsz, dt, attr, crc,
                               comptype, compqual, params, name, namelen, cm, cm_n);
        if (write_header(w->fp, body, (uint16_t)blen) != ACE_OK ||
            (packsize && fwrite(payload, 1, (size_t)packsize, w->fp) != (size_t)packsize))
            rc = ACE_ERR_IO;
        w->remain -= (size_t)packsize;
        if (w->remain == 0)
            vol_close(w);
        free(body);
        return rc;
    }
    while (off < packsize) {
        size_t take, blen;
        int more;
        uint16_t cf;
        if (w->remain == 0)
            vol_close(w);
        if (vol_ensure(w) != ACE_OK) {
            free(body);
            return ACE_ERR_IO;
        }
        take = w->remain < (size_t)(packsize - off) ? w->remain : (size_t)(packsize - off);
        if (take == 0)
            take = (size_t)(packsize - off);
        more = (off + take < packsize);
        cf = hflags;
        if (!firstpart)
            cf |= ACE_FLAG_CONTPREV;
        if (more)
            cf |= ACE_FLAG_CONTNEXT;
        blen = build_file_body(body, cf, (uint64_t)take, origsz, dt, attr, crc,
                               comptype, compqual, params, name, namelen, cm, cm_n);
        if (write_header(w->fp, body, (uint16_t)blen) != ACE_OK ||
            fwrite(payload + off, 1, take, w->fp) != take) {
            rc = ACE_ERR_IO;
            break;
        }
        w->remain -= take;
        off += take;
        firstpart = 0;
    }
    if (rc == ACE_OK && w->remain == 0)
        vol_close(w);
    free(body);
    return rc;
}

/* Read, compress, encrypt and archive a single regular file member. */
static int archive_file(volw_t *w, const job_t *j, const char *diskpath,
                        const char *name, size_t namelen)
{
    FILE *in;
    long sz;
    uint8_t *data = NULL;
    uint8_t *payload;
    size_t packsize;
    uint32_t crc, dt = 0, attr = ACE_ATTR_ARCHIVE;
    uint8_t comptype = ACE_COMP_STORED;
    uint8_t compqual = ACE_QUAL_NONE;
    uint16_t fparams = 0;
    uint16_t hflags;
    ace_stat_t st;
    int rc;

    in = fopen(diskpath, "rb");
    if (!in) {
        perror(diskpath);
        return ACE_ERR_IO;
    }
    if (fseek(in, 0, SEEK_END) != 0) {
        fclose(in);
        return ACE_ERR_IO;
    }
    sz = ftell(in);
    if (sz < 0) {
        fclose(in);
        return ACE_ERR_IO;
    }
    rewind(in);
    data = (uint8_t *)malloc((size_t)sz ? (size_t)sz : 1);
    if (!data || fread(data, 1, (size_t)sz, in) != (size_t)sz) {
        free(data);
        fclose(in);
        return ACE_ERR_IO;
    }
    fclose(in);
    crc = ace_crc32(data, (size_t)sz);
    payload = data;
    packsize = (size_t)sz;
    if (j->method && sz > 0) {
        uint8_t *comp = NULL;
        size_t cn = 0;
        if (j->method == 2)
            rc = ace_compress_blocked_hist(data, (size_t)sz, j->dicsize, j->quality,
                                           j->force_mode, j->pic_width, j->pic_planes,
                                           j->solid ? j->hist : NULL, &comp, &cn);
        else
            rc = ace_compress_lz77_hist(data, (size_t)sz, j->dicsize, j->quality,
                                        j->solid ? j->hist : NULL, &comp, &cn);
        if (rc != ACE_OK) {
            fprintf(stderr, "%s: compress failed (%s)\n", diskpath, ace_strerror(rc));
            free(data);
            return rc;
        }
        if (cn >= (size_t)sz) {
            free(comp);
            comptype = ACE_COMP_STORED;
            compqual = (uint8_t)j->quality;
            fparams = j->params;
        } else {
            payload = comp;
            packsize = cn;
            comptype = (j->method == 2) ? ACE_COMP_BLOCKED : ACE_COMP_LZ77;
            compqual = (uint8_t)j->quality;
            fparams = j->params;
        }
    }
    if (j->password && packsize > 0) {
        size_t padded = (packsize + 7) & ~(size_t)7;
        uint8_t *eb = (uint8_t *)malloc(padded);
        ace_bf_t bf;
        size_t k;
        if (!eb) {
            if (payload != data)
                free(payload);
            free(data);
            return ACE_ERR_NOMEM;
        }
        memcpy(eb, payload, packsize);
        for (k = packsize; k < padded; k++)
            eb[k] = 0;
        ace_bf_init(&bf, (const uint8_t *)j->password, strlen(j->password));
        ace_bf_encrypt(&bf, eb, padded);
        if (payload != data)
            free(payload);
        payload = eb;
        packsize = padded;
    }
    if (ace_stat(diskpath, &st) == 0) {
        dt = dos_from_time(st.st_mtime);
#ifdef _WIN32
        attr = dos_attrs_path(diskpath, st.st_mode);
#else
        attr = dos_attrs(st.st_mode);
#endif
    }
    hflags = ACE_FLAG_ADDSIZE;
    if (j->solid)
        hflags |= ACE_FLAG_SOLID;
    if (j->password)
        hflags |= ACE_FLAG_PASSWORD;
    if (j->file_cm_n)
        hflags |= ACE_FLAG_COMMENT;

    rc = emit_entry(w, hflags, payload, packsize, (uint64_t)sz, dt, attr, crc,
                    comptype, compqual, fparams, name, namelen, j->file_cm, j->file_cm_n);
    if (rc == ACE_OK && j->solid && j->method == 0)
        ace_comp_hist_add(j->hist, data, (size_t)sz, j->dicsize);
    if (payload != data)
        free(payload);
    free(data);
    return rc;
}

/* Write a zero-length directory member carrying the ACE directory attribute. */
static int archive_dir(volw_t *w, const job_t *j, const char *diskpath,
                       const char *name, size_t namelen)
{
    ace_stat_t st;
    uint32_t dt = 0, attr = ACE_ATTR_DIRECTORY;
    (void)j;
    if (ace_stat(diskpath, &st) == 0) {
        dt = dos_from_time(st.st_mtime);
#ifdef _WIN32
        attr = dos_attrs_path(diskpath, st.st_mode);
#else
        attr = dos_attrs(st.st_mode);
#endif
    }
    attr |= ACE_ATTR_DIRECTORY;
    return emit_entry(w, ACE_FLAG_ADDSIZE, NULL, 0, 0, dt, attr, 0,
                      ACE_COMP_STORED, ACE_QUAL_NONE, 0, name, namelen, NULL, 0);
}

/* Recursively archive a directory: emit its entry, then every file/subdir. */
static int walk_dir(volw_t *w, const job_t *j, const char *dirpath,
                    const char *name, size_t namelen)
{
    DIR *d;
    struct dirent *de;
    int rc;

    if (is_excluded(j->excludes, dirpath, name))
        return ACE_OK;

    rc = archive_dir(w, j, dirpath, name, namelen);
    if (rc != ACE_OK)
        return rc;
    d = opendir(dirpath);
    if (!d) {
        perror(dirpath);
        return ACE_ERR_IO;
    }
    while ((de = readdir(d)) != NULL) {
        char childpath[4096];
        char childname[4096];
        ace_stat_t cs;
        int cn;
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(childpath, sizeof(childpath), "%s/%s", dirpath, de->d_name) >= (int)sizeof(childpath))
            continue;
        cn = snprintf(childname, sizeof(childname), "%.*s\\%s", (int)namelen, name, de->d_name);
        if (cn < 0 || (size_t)cn >= sizeof(childname))
            continue;
        if (is_excluded(j->excludes, childpath, childname))
            continue;
        if (ace_stat(childpath, &cs) != 0)
            continue;
        if (S_ISDIR(cs.st_mode)) {
            rc = walk_dir(w, j, childpath, childname, (size_t)cn);
        } else if (S_ISREG(cs.st_mode)) {
            rc = archive_file(w, j, childpath, childname, (size_t)cn);
        }
        if (rc != ACE_OK) {
            closedir(d);
            return rc;
        }
    }
    closedir(d);
    return ACE_OK;
}

/* Dispatch one command-line input: a directory is walked recursively, a
 * regular file is archived directly. */
static int add_input(volw_t *w, const job_t *j, const char *arg)
{
    ace_stat_t st;
    char name[4096];
    size_t nlen;

    if (ace_stat(arg, &st) != 0) {
        perror(arg);
        return ACE_ERR_IO;
    }
    nlen = stored_name(arg, j->store_paths, name, sizeof(name));
    if (nlen == 0)
        return ACE_ERR_PARAM;
    if (is_excluded(j->excludes, arg, name))
        return ACE_OK;
    if (S_ISDIR(st.st_mode))
        return walk_dir(w, j, arg, name, nlen);
    if (S_ISREG(st.st_mode))
        return archive_file(w, j, arg, name, nlen);
    return ACE_OK;
}

/* Dispatch input from command line or from a list file (@listfile). */
static int add_input_or_list(volw_t *w, const job_t *j, const char *arg)
{
    if (arg[0] == '@') {
        const char *listpath = arg + 1;
        FILE *fp = (strcmp(listpath, "-") == 0) ? stdin : fopen(listpath, "r");
        char line[4096];
        if (!fp) {
            perror(listpath);
            return ACE_ERR_IO;
        }
        while (fgets(line, sizeof(line), fp)) {
            size_t len = strlen(line);
            while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                               line[len - 1] == ' ' || line[len - 1] == '\t'))
                line[--len] = '\0';
            if (len > 0 && line[0] != '#') {
                int rc = add_input_or_list(w, j, line);
                if (rc != ACE_OK) {
                    if (fp != stdin)
                        fclose(fp);
                    return rc;
                }
            }
        }
        if (fp != stdin)
            fclose(fp);
        return ACE_OK;
    }
    return add_input(w, j, arg);
}

int mkace_main(int argc, char **argv)
{
    const char *outpath = NULL;
    int i, first;
    int method = 0;
    unsigned force_mode = 0;
    int pic_width = 0;
    int pic_planes = 1;
    uint32_t dt = dos_now();
    const char *ts_env = getenv("ACE_TIMESTAMP");
    if (ts_env && *ts_env)
        dt = (uint32_t)strtoul(ts_env, NULL, 0);
    else if (getenv("SOURCE_DATE_EPOCH"))
        dt = dos_from_time((time_t)strtoull(getenv("SOURCE_DATE_EPOCH"), NULL, 10));
    uint16_t flags = ACE_FLAG_ADVERT;
    size_t dicsize = 1048576;
    uint16_t params = 10;
    unsigned quality = ACE_QUAL_NORMAL;
    int solid = 0;
    int locked = 0;
    ace_sfx_type_t sfx_type = ACE_SFX_NONE;
    int store_paths = 0;
    size_t volsize = 0;
    const char *password = NULL;
    const char *main_comment = NULL;
    const char *file_comment = NULL;
    uint8_t *main_cm = NULL;
    size_t main_cm_n = 0;
    uint8_t *file_cm = NULL;
    size_t file_cm_n = 0;
    exclude_list_t excludes = {0};
    volcfg_t cfg;
    volw_t w;
    job_t j;
    ace_comp_hist_t hist;

    ace_comp_hist_init(&hist);
    memset(&w, 0, sizeof(w));
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc)
            outpath = argv[++i];
        else if (strcmp(argv[i], "-0") == 0)
            method = 0;
        else if (strcmp(argv[i], "-z") == 0)
            method = 1;
        else if (strcmp(argv[i], "-2") == 0)
            method = 2;
        else if (strcmp(argv[i], "-m") == 0 && i + 1 < argc) {
            long q = strtol(argv[++i], NULL, 10);
            if (q < 0 || q > 5) {
                usage(argv[0]);
                exclude_free(&excludes);
                return 1;
            }
            quality = (unsigned)q;
            if (quality == ACE_QUAL_NONE)
                method = 0;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            long kb = strtol(argv[++i], NULL, 10);
            unsigned bits;
            if (kb != 32 && kb != 64 && kb != 128 && kb != 256 &&
                kb != 512 && kb != 1024 && kb != 2048 && kb != 4096) {
                usage(argv[0]);
                exclude_free(&excludes);
                return 1;
            }
            dicsize = (size_t)kb * 1024;
            bits = 0;
            while ((1u << bits) < (unsigned)kb)
                bits++;
            params = (uint16_t)bits;
        } else if (strcmp(argv[i], "-V") == 0 && i + 1 < argc) {
            long long vb = strtoll(argv[++i], NULL, 10);
            if (vb < 1) {
                usage(argv[0]);
                exclude_free(&excludes);
                return 1;
            }
            volsize = (size_t)vb;
        } else if (strcmp(argv[i], "-A") == 0) {
            store_paths = 1;
        } else if (strcmp(argv[i], "-k") == 0) {
            locked = 1;
        } else if (strcmp(argv[i], "-pw") == 0 && i + 1 < argc)
            password = argv[++i];
        else if (strcmp(argv[i], "-cm") == 0 && i + 1 < argc)
            main_comment = argv[++i];
        else if (strcmp(argv[i], "-cf") == 0 && i + 1 < argc)
            file_comment = argv[++i];
        else if (strcmp(argv[i], "-s") == 0)
            solid = 1;
        else if (strcmp(argv[i], "-s8") == 0) {
            method = 2;
            force_mode = ACE_MODE_SOUND_8;
        } else if (strcmp(argv[i], "-s16") == 0) {
            method = 2;
            force_mode = ACE_MODE_SOUND_16;
        } else if (strcmp(argv[i], "-s32a") == 0) {
            method = 2;
            force_mode = ACE_MODE_SOUND_32A;
        } else if (strcmp(argv[i], "-s32b") == 0) {
            method = 2;
            force_mode = ACE_MODE_SOUND_32B;
        } else if (strcmp(argv[i], "-xe") == 0) {
            method = 2;
            force_mode = ACE_MODE_LZ77_EXE;
        } else if (strcmp(argv[i], "-dl") == 0) {
            method = 2;
            force_mode = ACE_MODE_LZ77_DELTA;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            char *colon;
            method = 2;
            force_mode = ACE_MODE_PIC;
            pic_width = (int)strtol(argv[++i], &colon, 10);
            pic_planes = 1;
            if (colon && *colon == ':')
                pic_planes = (int)strtol(colon + 1, NULL, 10);
            if (pic_width <= 0 || pic_planes <= 0) {
                usage(argv[0]);
                exclude_free(&excludes);
                return 1;
            }
        } else if (strcmp(argv[i], "-x") == 0 && i + 1 < argc) {
            i++;
            if (argv[i][0] == '@')
                exclude_load_file(&excludes, argv[i] + 1);
            else
                exclude_add(&excludes, argv[i]);
        } else if (strncmp(argv[i], "-x", 2) == 0 && argv[i][2] != '\0') {
            if (argv[i][2] == '@')
                exclude_load_file(&excludes, argv[i] + 3);
            else
                exclude_add(&excludes, argv[i] + 2);
        } else if (strcmp(argv[i], "-sfx-") == 0) {
            sfx_type = ACE_SFX_NONE;
        } else if (strcmp(argv[i], "-sfx") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                ace_sfx_type_t t = ace_sfx_parse_type(argv[i + 1]);
                if (t != ACE_SFX_NONE) {
                    sfx_type = t;
                    i++;
                } else {
                    sfx_type = ACE_SFX_DEFAULT_TYPE;
                }
            } else {
                sfx_type = ACE_SFX_DEFAULT_TYPE;
            }
        } else if (strncmp(argv[i], "-sfx=", 5) == 0) {
            sfx_type = ace_sfx_parse_type(argv[i] + 5);
            if (sfx_type == ACE_SFX_NONE)
                sfx_type = ACE_SFX_DEFAULT_TYPE;
        } else if (strncmp(argv[i], "-sfx", 4) == 0 && argv[i][4] != '\0') {
            sfx_type = ace_sfx_parse_type(argv[i] + 4);
            if (sfx_type == ACE_SFX_NONE)
                sfx_type = ACE_SFX_DEFAULT_TYPE;
        } else if (argv[i][0] == '-') {
            usage(argv[0]);
            exclude_free(&excludes);
            return 1;
        } else {
            break;
        }
    }
    first = i;
    static char auto_sfx_out[4096];
    if (sfx_type != ACE_SFX_NONE && outpath) {
        size_t olen = strlen(outpath);
        const char *ext = (sfx_type == ACE_SFX_LINUX) ? ".sfx" : ".exe";
        if (olen > 4 && strcasecmp(outpath + olen - 4, ".ace") == 0) {
            snprintf(auto_sfx_out, sizeof(auto_sfx_out), "%.*s%s", (int)(olen - 4), outpath, ext);
            outpath = auto_sfx_out;
        }
    }
    if (!outpath || first >= argc) {
        usage(argv[0]);
        exclude_free(&excludes);
        return 1;
    }
    if (solid)
        flags |= ACE_FLAG_SOLID;
    if (locked)
        flags |= ACE_FLAG_LOCKED;

    if (main_comment && main_comment[0]) {
        int rc = ace_compress_comment((const uint8_t *)main_comment, strlen(main_comment),
                                      &main_cm, &main_cm_n);
        if (rc != ACE_OK) {
            fprintf(stderr, "main comment: %s\n", ace_strerror(rc));
            exclude_free(&excludes);
            return 1;
        }
        flags |= ACE_FLAG_COMMENT;
    }
    if (file_comment && file_comment[0]) {
        int rc = ace_compress_comment((const uint8_t *)file_comment, strlen(file_comment),
                                      &file_cm, &file_cm_n);
        if (rc != ACE_OK) {
            fprintf(stderr, "file comment: %s\n", ace_strerror(rc));
            exclude_free(&excludes);
            free(main_cm);
            return 1;
        }
    }

    cfg.outpath = outpath;
    cfg.main_flags = flags;
    cfg.dt = dt;
    cfg.main_cm = main_cm;
    cfg.main_cm_n = main_cm_n;
    cfg.multivol = (volsize > 0);
    cfg.volsize = (volsize > 0) ? volsize : SIZE_MAX;
    cfg.method = method;
    cfg.host = (sfx_type == ACE_SFX_DOS) ? ACE_HOST_MSDOS :
               (sfx_type == ACE_SFX_LINUX) ? ACE_HOST_LINUX : ACE_HOST_WIN32;
    cfg.sfx_type = sfx_type;
    w.cfg = &cfg;

    memset(&j, 0, sizeof(j));
    j.method = method;
    j.quality = quality;
    j.dicsize = dicsize;
    j.params = params;
    j.force_mode = force_mode;
    j.pic_width = pic_width;
    j.pic_planes = pic_planes;
    j.solid = solid;
    j.store_paths = store_paths;
    j.password = password;
    j.file_cm = file_cm;
    j.file_cm_n = file_cm_n;
    j.hist = &hist;
    j.excludes = &excludes;

    for (i = first; i < argc; i++) {
        if (add_input_or_list(&w, &j, argv[i]) != ACE_OK) {
            vol_close(&w);
            ace_comp_hist_free(&hist);
            exclude_free(&excludes);
            free(main_cm);
            free(file_cm);
            fprintf(stderr, "%s: failed to archive %s\n", argv[0], argv[i]);
            return 1;
        }
    }

    vol_close(&w);
    ace_comp_hist_free(&hist);
    exclude_free(&excludes);
    free(main_cm);
    free(file_cm);
    return 0;
}
