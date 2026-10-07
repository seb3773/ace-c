#include "ace/util.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

const char *ace_strerror(int err)
{
    switch (err) {
    case ACE_OK:
        return "ok";
    case ACE_ERR_IO:
        return "i/o error";
    case ACE_ERR_NOMEM:
        return "out of memory";
    case ACE_ERR_CORRUPT:
        return "corrupted archive";
    case ACE_ERR_TRUNCATED:
        return "truncated archive";
    case ACE_ERR_CRC:
        return "crc mismatch";
    case ACE_ERR_PASSWORD:
        return "password required or incorrect";
    case ACE_ERR_METHOD:
        return "unknown compression method";
    case ACE_ERR_NOT_ACE:
        return "not an ACE archive";
    case ACE_ERR_MULTIVOL:
        return "multi-volume archive error";
    case ACE_ERR_PARAM:
        return "invalid parameter";
    case ACE_ERR_EOF:
        return "unexpected end of stream";
    default:
        return "unknown error";
    }
}

int ace_mkdir_p(const char *path)
{
    char tmp[4096];
    size_t len;
    size_t i;

    if (!path || !*path)
        return ACE_OK;
    len = strlen(path);
    if (len >= sizeof(tmp))
        return ACE_ERR_PARAM;
    memcpy(tmp, path, len + 1);
    for (i = 1; i < len; i++) {
        if (tmp[i] == '/') {
            tmp[i] = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
                return ACE_ERR_IO;
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST)
        return ACE_ERR_IO;
    return ACE_OK;
}

static int is_reserved_char(unsigned char c)
{
    if (c < 32)
        return 1;
    switch (c) {
    case ':':
    case '<':
    case '>':
    case '"':
    case '?':
    case '*':
    case '|':
        return 1;
    default:
        return 0;
    }
}

int ace_is_safe_relpath(const char *path)
{
    if (!path || !path[0])
        return 0;
    if (path[0] == '/' || path[0] == '\\')
        return 0;
    if (isalpha((unsigned char)path[0]) && path[1] == ':')
        return 0;

    int depth = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/' || *p == '\\')
            p++;
        if (!*p)
            break;
        const char *start = p;
        while (*p && *p != '/' && *p != '\\')
            p++;
        size_t len = (size_t)(p - start);
        if (len == 1 && start[0] == '.')
            continue;
        if (len == 2 && start[0] == '.' && start[1] == '.') {
            depth--;
            if (depth < 0)
                return 0;
        } else {
            if (len >= 2 && isalpha((unsigned char)start[0]) && start[1] == ':')
                return 0;
            depth++;
        }
    }
    return depth > 0;
}

void ace_normalize_path_separators(char *path)
{
    if (!path)
        return;
    for (char *p = path; *p; p++) {
        if (*p == '\\')
            *p = '/';
    }
}

void ace_format_unix_path(const char *src, char *dst, size_t dst_sz)
{
    if (!src || !dst || dst_sz == 0)
        return;
    size_t i;
    for (i = 0; i + 1 < dst_sz && src[i]; i++) {
        char c = src[i];
        if (c == '\\')
            c = '/';
        dst[i] = (char)tolower((unsigned char)c);
    }
    dst[i] = '\0';
}

int ace_sanitize_path(const uint8_t *raw, size_t raw_len, char *out, size_t out_sz)
{
    char tmp[4096];
    size_t i, j, n;
    char *parts[256];
    int nparts = 0;
    char *save;
    char *tok;
    size_t out_i;

    if (!raw || !out || out_sz == 0)
        return ACE_ERR_PARAM;

    n = raw_len < sizeof(tmp) - 1 ? raw_len : sizeof(tmp) - 1;
    j = 0;
    for (i = 0; i < n; i++) {
        unsigned char c = raw[i];
        if (c == 0)
            break;
        if (c == '\\')
            c = '/';
        if (is_reserved_char(c))
            continue;
        tmp[j++] = (char)c;
    }
    tmp[j] = 0;

    save = tmp;
    while ((tok = strtok(save, "/")) != NULL) {
        save = NULL;
        if (tok[0] == 0 || (tok[0] == '.' && tok[1] == 0))
            continue;
        if (tok[0] == '.' && tok[1] == '.' && tok[2] == 0) {
            if (nparts > 0)
                nparts--;
            continue;
        }
        if (nparts < (int)(sizeof(parts) / sizeof(parts[0])))
            parts[nparts++] = tok;
    }

    out_i = 0;
    if (nparts == 0) {
        if (out_sz < 2)
            return ACE_ERR_PARAM;
        out[0] = 0;
        return ACE_OK;
    }
    for (i = 0; i < (size_t)nparts; i++) {
        size_t pl = strlen(parts[i]);
        if (out_i + pl + 2 >= out_sz)
            return ACE_ERR_PARAM;
        if (i)
            out[out_i++] = '/';
        memcpy(out + out_i, parts[i], pl);
        out_i += pl;
    }
    out[out_i] = 0;
    return ACE_OK;
}
