#include "ace/archive.h"
#include "ace/blowfish.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

static const char *host_names[] = {
    "MS-DOS", "OS/2", "Win32", "Unix", "Mac OS", "Win NT", "Primos",
    "Apple GS", "ATARI", "VAX VMS", "AMIGA", "NeXT", "Linux"
};

const char *ace_host_str(unsigned host)
{
    if (host < sizeof(host_names) / sizeof(host_names[0]))
        return host_names[host];
    return "?";
}

const char *ace_comp_str(unsigned t)
{
    switch (t) {
    case ACE_COMP_STORED:
        return "stored";
    case ACE_COMP_LZ77:
        return "lz77";
    case ACE_COMP_BLOCKED:
        return "blocked";
    default:
        return "?";
    }
}

const char *ace_qual_str(unsigned q)
{
    static const char *n[] = {"store", "fastest", "fast", "normal", "good", "best"};
    if (q < 6)
        return n[q];
    return "?";
}

static int members_push(ace_archive_t *ar, const ace_filehdr_t *hdr)
{
    if (ar->nmembers >= ar->members_cap) {
        size_t cap = ar->members_cap ? ar->members_cap * 2 : 16;
        ace_member_t *n = (ace_member_t *)realloc(ar->members, cap * sizeof(*n));
        if (!n)
            return ACE_ERR_NOMEM;
        ar->members = n;
        ar->members_cap = cap;
    }
    memset(&ar->members[ar->nmembers], 0, sizeof(ar->members[0]));
    ar->members[ar->nmembers].hdr = *hdr;
    ar->members[ar->nmembers].fp = ar->cur_fp;
    ar->members[ar->nmembers].segs = NULL;
    ar->members[ar->nmembers].nsegs = 0;
    ar->members[ar->nmembers].incomplete = 0;
    ar->nmembers++;
    return ACE_OK;
}

static int member_add_seg(ace_member_t *m, FILE *fp, uint64_t offset, uint64_t size)
{
    ace_seg_t *n;
    n = (ace_seg_t *)realloc(m->segs, (m->nsegs + 1) * sizeof(*n));
    if (!n)
        return ACE_ERR_NOMEM;
    m->segs = n;
    m->segs[m->nsegs].fp = fp;
    m->segs[m->nsegs].offset = offset;
    m->segs[m->nsegs].size = size;
    m->nsegs++;
    return ACE_OK;
}

static int vols_push(ace_archive_t *ar, FILE *fp, const char *path, uint64_t filesize, uint8_t volume)
{
    if (ar->nvols >= ar->vols_cap) {
        size_t cap = ar->vols_cap ? ar->vols_cap * 2 : 4;
        ace_vol_t *n = (ace_vol_t *)realloc(ar->vols, cap * sizeof(*n));
        if (!n)
            return ACE_ERR_NOMEM;
        ar->vols = n;
        ar->vols_cap = cap;
    }
    memset(&ar->vols[ar->nvols], 0, sizeof(ar->vols[0]));
    snprintf(ar->vols[ar->nvols].path, sizeof(ar->vols[ar->nvols].path), "%s", path);
    ar->vols[ar->nvols].fp = fp;
    ar->vols[ar->nvols].filesize = filesize;
    ar->vols[ar->nvols].volume = volume;
    ar->nvols++;
    return ACE_OK;
}

static void vol_join(char *dst, size_t dst_n, const char *base, const char *suffix)
{
    size_t blen, slen;

    if (dst_n == 0)
        return;
    blen = strlen(base);
    slen = strlen(suffix);
    if (blen + slen + 1 > dst_n) {
        blen = (slen + 1 < dst_n) ? dst_n - slen - 1 : 0;
        slen = dst_n - blen - 1;
    }
    memcpy(dst, base, blen);
    memcpy(dst + blen, suffix, slen);
    dst[blen + slen] = 0;
}

static void next_volume_names(const char *path, char *lo, size_t lo_n, char *hi, size_t hi_n)
{
    const char *dot = strrchr(path, '.');
    char base[4096];
    char sfx[16];
    size_t blen;

    if (!dot || dot == path) {
        vol_join(lo, lo_n, path, ".c00");
        vol_join(hi, hi_n, path, ".C00");
        return;
    }
    blen = (size_t)(dot - path);
    if (blen >= sizeof(base))
        blen = sizeof(base) - 1;
    memcpy(base, path, blen);
    base[blen] = 0;
    if ((dot[1] == 'c' || dot[1] == 'C') && dot[2] != 0) {
        char *end = NULL;
        long n = strtol(dot + 2, &end, 10);
        if (end && *end == 0 && n >= 0 && n < 1000000) {
            snprintf(sfx, sizeof(sfx), ".c%02d", (int)n + 1);
            vol_join(lo, lo_n, base, sfx);
            snprintf(sfx, sizeof(sfx), ".C%02d", (int)n + 1);
            vol_join(hi, hi_n, base, sfx);
            return;
        }
    }
    vol_join(lo, lo_n, base, ".c00");
    vol_join(hi, hi_n, base, ".C00");
}

static int read_fully(FILE *fp, void *buf, size_t n)
{
    if (n == 0)
        return ACE_OK;
    if (fread(buf, 1, n, fp) != n)
        return ACE_ERR_TRUNCATED;
    return ACE_OK;
}

static int parse_one_header(ace_archive_t *ar, int expect_main)
{
    uint8_t prefix[4];
    uint16_t hcrc, hsize;
    uint8_t *buf;
    uint8_t htype;
    uint16_t hflags;
    size_t i;
    int rc;

    rc = read_fully(ar->cur_fp, prefix, 4);
    if (rc != ACE_OK)
        return rc;
    hcrc = ace_r16(prefix);
    hsize = ace_r16(prefix + 2);
    if (hsize < 3)
        return ACE_ERR_CORRUPT;
    buf = (uint8_t *)malloc(hsize);
    if (!buf)
        return ACE_ERR_NOMEM;
    rc = read_fully(ar->cur_fp, buf, hsize);
    if (rc != ACE_OK) {
        free(buf);
        return rc;
    }
    if (ace_crc16(buf, hsize) != hcrc) {
        free(buf);
        return ACE_ERR_CORRUPT;
    }
    htype = buf[0];
    hflags = ace_r16(buf + 1);
    i = 3;

    if (htype == ACE_TYPE_MAIN) {
        ace_mainhdr_t *m = &ar->main;
        uint8_t avsz;
        if (hflags & ACE_FLAG_ADDSIZE) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        if (i + 23 > hsize) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        memcpy(m->magic, buf + 3, 7);
        if (memcmp(m->magic, ACE_MAGIC, 7) != 0) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        m->hdr_crc = hcrc;
        m->hdr_size = hsize;
        m->hdr_type = htype;
        m->hdr_flags = hflags;
        m->eversion = buf[10];
        m->cversion = buf[11];
        m->host = buf[12];
        m->volume = buf[13];
        m->datetime = ace_r32(buf + 14);
        memcpy(m->reserved1, buf + 18, 8);
        i = 26;
        if (i + 1 > hsize) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        avsz = buf[i++];
        if (i + avsz > hsize) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        if (avsz) {
            m->advert = (uint8_t *)malloc(avsz);
            if (!m->advert) {
                free(buf);
                return ACE_ERR_NOMEM;
            }
            memcpy(m->advert, buf + i, avsz);
            m->advert_len = avsz;
        }
        i += avsz;
        if ((hflags & ACE_FLAG_ADVERT) && avsz == 0) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        if (!(hflags & ACE_FLAG_ADVERT) && avsz > 0) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        if (hflags & ACE_FLAG_COMMENT) {
            uint16_t cmsz;
            if (i + 2 > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            cmsz = ace_r16(buf + i);
            i += 2;
            if (i + cmsz > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            rc = ace_decompress_comment(buf + i, cmsz, &m->comment, &m->comment_len);
            if (rc != ACE_OK) {
                free(buf);
                return rc;
            }
            i += cmsz;
        }
        ar->solid = (hflags & ACE_FLAG_SOLID) != 0;
        ar->multivolume = (hflags & ACE_FLAG_MULTIVOLUME) != 0;
        ar->locked = (hflags & ACE_FLAG_LOCKED) != 0;
        (void)expect_main;
    } else if (htype == ACE_TYPE_FILE32 || htype == ACE_TYPE_FILE64) {
        ace_filehdr_t fh;
        uint16_t fnsz;
        memset(&fh, 0, sizeof(fh));
        fh.hdr_crc = hcrc;
        fh.hdr_size = hsize;
        fh.hdr_type = htype;
        fh.hdr_flags = hflags;
        if (!(hflags & ACE_FLAG_ADDSIZE)) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        if (hflags & ACE_FLAG_64BIT) {
            if (htype != ACE_TYPE_FILE64 || i + 16 > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            fh.packsize = ace_r64(buf + i);
            fh.origsize = ace_r64(buf + i + 8);
            i += 16;
        } else {
            if (htype != ACE_TYPE_FILE32 || i + 8 > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            fh.packsize = ace_r32(buf + i);
            fh.origsize = ace_r32(buf + i + 4);
            i += 8;
        }
        if (i + 20 > hsize) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        fh.datetime = ace_r32(buf + i);
        fh.attribs = ace_r32(buf + i + 4);
        fh.crc32 = ace_r32(buf + i + 8);
        fh.comptype = buf[i + 12];
        fh.compqual = buf[i + 13];
        fh.params = ace_r16(buf + i + 14);
        fh.reserved1 = ace_r16(buf + i + 16);
        fnsz = ace_r16(buf + i + 18);
        i += 20;
        if (i + fnsz > hsize) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        fh.raw_filename_len = fnsz < sizeof(fh.raw_filename) ? fnsz : sizeof(fh.raw_filename) - 1;
        memcpy(fh.raw_filename, buf + i, fh.raw_filename_len);
        fh.raw_filename[fh.raw_filename_len] = '\0';
        size_t nlen = fh.raw_filename_len < sizeof(fh.filename) - 1 ? fh.raw_filename_len : sizeof(fh.filename) - 1;
        memcpy(fh.filename, fh.raw_filename, nlen);
        fh.filename[nlen] = '\0';
        ace_normalize_path_separators(fh.filename);
        if (fh.filename[0] == 0)
            snprintf(fh.filename, sizeof(fh.filename), "file%04zu", ar->nmembers);
        i += fnsz;
        if (hflags & ACE_FLAG_COMMENT) {
            uint16_t cmsz;
            if (i + 2 > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            cmsz = ace_r16(buf + i);
            i += 2;
            if (i + cmsz > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            rc = ace_decompress_comment(buf + i, cmsz, &fh.comment, &fh.comment_len);
            if (rc != ACE_OK) {
                free(buf);
                return rc;
            }
            i += cmsz;
        }
        if (hflags & ACE_FLAG_NTSECURITY) {
            /* NT security descriptors are a Windows/NTFS-only concept and are
             * out of scope for this Linux port. We only surface the raw blob
             * (see -H dump); applying it back would require Win32 APIs. */
            uint16_t nssz;
            if (i + 2 > hsize) {
                free(fh.comment);
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            nssz = ace_r16(buf + i);
            i += 2;
            if (i + nssz > hsize) {
                free(fh.comment);
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            fh.ntsecurity = (uint8_t *)malloc(nssz);
            if (!fh.ntsecurity) {
                free(fh.comment);
                free(buf);
                return ACE_ERR_NOMEM;
            }
            memcpy(fh.ntsecurity, buf + i, nssz);
            fh.ntsecurity_len = nssz;
            i += nssz;
        }
        fh.dataoffset = (uint64_t)ftell(ar->cur_fp);
        if (fseek(ar->cur_fp, (long)fh.packsize, SEEK_CUR) != 0) {
            free(fh.comment);
            free(fh.ntsecurity);
            free(buf);
            return ACE_ERR_TRUNCATED;
        }
        rc = members_push(ar, &fh);
        if (rc != ACE_OK) {
            free(fh.comment);
            free(fh.ntsecurity);
            free(buf);
            return rc;
        }
        rc = member_add_seg(&ar->members[ar->nmembers - 1], ar->cur_fp, fh.dataoffset, fh.packsize);
        if (rc != ACE_OK) {
            free(buf);
            return rc;
        }
        (void)i;
    } else if (htype == ACE_TYPE_RECOVERY32 || htype == ACE_TYPE_RECOVERY64A ||
               htype == ACE_TYPE_RECOVERY64B) {
        uint64_t addsz = 0;
        if (!(hflags & ACE_FLAG_ADDSIZE)) {
            free(buf);
            return ACE_ERR_CORRUPT;
        }
        if (hflags & ACE_FLAG_64BIT) {
            if (i + 8 > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            addsz = ace_r64(buf + i);
        } else {
            if (i + 4 > hsize) {
                free(buf);
                return ACE_ERR_CORRUPT;
            }
            addsz = ace_r32(buf + i);
        }
        if (fseek(ar->cur_fp, (long)addsz, SEEK_CUR) != 0) {
            free(buf);
            return ACE_ERR_TRUNCATED;
        }
    } else {
        uint64_t addsz = 0;
        if (hflags & ACE_FLAG_ADDSIZE) {
            if (hflags & ACE_FLAG_64BIT) {
                if (i + 8 > hsize) {
                    free(buf);
                    return ACE_ERR_CORRUPT;
                }
                addsz = ace_r64(buf + i);
            } else {
                if (i + 4 > hsize) {
                    free(buf);
                    return ACE_ERR_CORRUPT;
                }
                addsz = ace_r32(buf + i);
            }
            if (fseek(ar->cur_fp, (long)addsz, SEEK_CUR) != 0) {
                free(buf);
                return ACE_ERR_TRUNCATED;
            }
        }
    }
    free(buf);
    return ACE_OK;
}

static int find_main_header(ace_archive_t *ar, size_t search)
{
    uint8_t buf[512];
    size_t n;

    if (fseek(ar->cur_fp, 0, SEEK_SET) != 0)
        return ACE_ERR_IO;
    n = fread(buf, 1, sizeof(buf), ar->cur_fp);
    if (n >= 14 && memcmp(buf + 7, ACE_MAGIC, 7) == 0) {
        if (fseek(ar->cur_fp, 0, SEEK_SET) != 0)
            return ACE_ERR_IO;
        if (parse_one_header(ar, 1) == ACE_OK)
            return ACE_OK;
    }
    if (search == 0)
        return ACE_ERR_NOT_ACE;
    if (fseek(ar->cur_fp, 0, SEEK_SET) != 0)
        return ACE_ERR_IO;
    {
        uint8_t *big = (uint8_t *)malloc(search);
        size_t got;
        size_t i;
        if (!big)
            return ACE_ERR_NOMEM;
        got = fread(big, 1, search, ar->cur_fp);
        for (i = 7; i + 7 <= got; i++) {
            if (memcmp(big + i, ACE_MAGIC, 7) == 0) {
                if (fseek(ar->cur_fp, (long)(i - 7), SEEK_SET) != 0) {
                    free(big);
                    return ACE_ERR_IO;
                }
                if (parse_one_header(ar, 1) == ACE_OK) {
                    free(big);
                    return ACE_OK;
                }
            }
        }
        free(big);
        return ACE_ERR_NOT_ACE;
    }
}

static int parse_volume_headers(ace_archive_t *ar)
{
    uint64_t pos;
    long start = ftell(ar->cur_fp);
    if (start < 0)
        return ACE_ERR_IO;
    if (fseek(ar->cur_fp, 0, SEEK_END) != 0)
        return ACE_ERR_IO;
    pos = (uint64_t)ftell(ar->cur_fp);
    if (fseek(ar->cur_fp, start, SEEK_SET) != 0)
        return ACE_ERR_IO;
    while ((uint64_t)ftell(ar->cur_fp) < pos) {
        long before = ftell(ar->cur_fp);
        int rc;
        if (before < 0 || (uint64_t)before + 4 > pos)
            break;
        rc = parse_one_header(ar, 0);
        if (rc != ACE_OK) {
            if (rc == ACE_ERR_TRUNCATED || rc == ACE_ERR_CORRUPT)
                break;
            return rc;
        }
    }
    return ACE_OK;
}

static int merge_members(ace_archive_t *ar)
{
    size_t i, out = 0;
    for (i = 0; i < ar->nmembers; i++) {
        ace_member_t *cur = &ar->members[i];
        if (cur->hdr.hdr_flags & ACE_FLAG_CONTPREV) {
            if (out == 0) {
                continue;
            }
            {
                ace_member_t *prev = &ar->members[out - 1];
                size_t k;
                for (k = 0; k < cur->nsegs; k++) {
                    int rc = member_add_seg(prev, cur->segs[k].fp,
                                            cur->segs[k].offset, cur->segs[k].size);
                    if (rc != ACE_OK)
                        return rc;
                }
                prev->hdr.packsize += cur->hdr.packsize;
                prev->incomplete = (cur->hdr.hdr_flags & ACE_FLAG_CONTNEXT) != 0;
                free(cur->segs);
                cur->segs = NULL;
                cur->nsegs = 0;
                free(cur->hdr.comment);
                free(cur->hdr.ntsecurity);
                cur->hdr.comment = NULL;
                cur->hdr.ntsecurity = NULL;
                continue;
            }
        }
        if (out != i) {
            ar->members[out] = ar->members[i];
            memset(&ar->members[i], 0, sizeof(ar->members[i]));
        }
        ar->members[out].incomplete = (cur->hdr.hdr_flags & ACE_FLAG_CONTNEXT) != 0;
        out++;
    }
    ar->nmembers = out;
    for (i = 0; i < out; i++)
        ar->members[i].hdr.hdr_flags &= ~(ACE_FLAG_CONTNEXT | ACE_FLAG_CONTPREV);
    return ACE_OK;
}

int ace_archive_open(ace_archive_t *ar, const char *path, size_t search)
{
    int rc;

    memset(ar, 0, sizeof(*ar));
    if (!path)
        return ACE_ERR_PARAM;
    snprintf(ar->path, sizeof(ar->path), "%s", path);
    ar->fp = fopen(path, "rb");
    if (!ar->fp)
        return ACE_ERR_IO;
    if (fseek(ar->fp, 0, SEEK_END) != 0) {
        fclose(ar->fp);
        ar->fp = NULL;
        return ACE_ERR_IO;
    }
    ar->filesize = (uint64_t)ftell(ar->fp);
    ar->cur_fp = ar->fp;
    rc = find_main_header(ar, search ? search : 524288);
    if (rc != ACE_OK) {
        ace_archive_close(ar);
        return rc;
    }
    rc = vols_push(ar, ar->fp, path, ar->filesize, ar->main.volume);
    if (rc != ACE_OK) {
        ace_archive_close(ar);
        return rc;
    }
    rc = parse_volume_headers(ar);
    if (rc != ACE_OK) {
        ace_archive_close(ar);
        return rc;
    }
    if (ar->main.hdr_flags & ACE_FLAG_MULTIVOLUME) {
        char lo[4096], hi[4096], cur[4096];
        snprintf(cur, sizeof(cur), "%s", path);
        while (1) {
            FILE *nfp;
            uint8_t vol;
            next_volume_names(cur, lo, sizeof(lo), hi, sizeof(hi));
            nfp = fopen(lo, "rb");
            if (!nfp)
                nfp = fopen(hi, "rb");
            if (!nfp)
                break;
            if (fseek(nfp, 0, SEEK_END) != 0) {
                fclose(nfp);
                break;
            }
            ar->filesize = (uint64_t)ftell(nfp);
            rewind(nfp);
            ar->cur_fp = nfp;
            rc = find_main_header(ar, 0);
            if (rc != ACE_OK) {
                fclose(nfp);
                ar->cur_fp = ar->fp;
                break;
            }
            vol = ar->main.volume;
            if (vols_push(ar, nfp, lo, ar->filesize, vol) != ACE_OK) {
                fclose(nfp);
                ar->cur_fp = ar->fp;
                break;
            }
            rc = parse_volume_headers(ar);
            if (rc != ACE_OK) {
                ar->cur_fp = ar->fp;
                break;
            }
            snprintf(cur, sizeof(cur), "%s", lo);
            if (ar->nmembers == 0 ||
                !(ar->members[ar->nmembers - 1].hdr.hdr_flags & ACE_FLAG_CONTNEXT)) {
                /* last member is complete; no further volume needed */
                break;
            }
        }
        ar->cur_fp = ar->fp;
    }
    rc = merge_members(ar);
    if (rc != ACE_OK) {
        ace_archive_close(ar);
        return rc;
    }
    rc = ace_engine_init(&ar->engine);
    if (rc != ACE_OK) {
        ace_archive_close(ar);
        return rc;
    }
    ar->engine_ready = 1;
    return ACE_OK;
}

void ace_archive_close(ace_archive_t *ar)
{
    size_t i;
    if (!ar)
        return;
    if (ar->engine_ready)
        ace_engine_free(&ar->engine);
    ar->engine_ready = 0;
    for (i = 0; i < ar->nmembers; i++) {
        free(ar->members[i].hdr.comment);
        free(ar->members[i].hdr.ntsecurity);
        free(ar->members[i].segs);
    }
    free(ar->members);
    for (i = 0; i < ar->nvols; i++) {
        if (ar->vols[i].fp && ar->vols[i].fp != ar->fp)
            fclose(ar->vols[i].fp);
    }
    free(ar->vols);
    free(ar->main.advert);
    free(ar->main.comment);
    if (ar->fp)
        fclose(ar->fp);
    memset(ar, 0, sizeof(*ar));
}

typedef struct {
    ace_crc32_t crc;
    ace_out_cb user;
    void *user_ctx;
} ace_crc_out_t;

static int crc_out_cb(void *ctx, const uint8_t *buf, size_t n)
{
    ace_crc_out_t *c = (ace_crc_out_t *)ctx;
    ace_crc32_update(&c->crc, buf, n);
    if (c->user)
        return c->user(c->user_ctx, buf, n);
    return ACE_OK;
}

typedef struct {
    ace_seg_t *segs;
    size_t nsegs;
    size_t idx;
    uint64_t off;
} ace_segsrc_t;

static size_t segsrc_read(void *ctx, void *buf, size_t n)
{
    ace_segsrc_t *s = (ace_segsrc_t *)ctx;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < n && s->idx < s->nsegs) {
        ace_seg_t *seg = &s->segs[s->idx];
        uint64_t avail = seg->size - s->off;
        size_t take;
        if (avail == 0) {
            s->idx++;
            s->off = 0;
            continue;
        }
        take = n - done;
        if ((uint64_t)take > avail)
            take = (size_t)avail;
        if (fseek(seg->fp, (long)(seg->offset + s->off), SEEK_SET) != 0)
            break;
        take = fread(out + done, 1, take, seg->fp);
        if (take == 0)
            break;
        s->off += take;
        done += take;
    }
    return done;
}

typedef struct {
    ace_segsrc_t src;
    uint64_t remain;
    ace_bf_t bf;
    int encrypted;
    uint8_t hold[8192];
    size_t hold_len;
    size_t hold_off;
} ace_packsrc_t;

static size_t packsrc_plain_read(void *ctx, void *buf, size_t n)
{
    ace_packsrc_t *s = (ace_packsrc_t *)ctx;
    size_t want = n;
    size_t got;
    if ((uint64_t)want > s->remain)
        want = (size_t)s->remain;
    want &= ~(size_t)3;
    if (want == 0)
        return 0;
    got = segsrc_read(&s->src, buf, want);
    s->remain -= got;
    return got;
}

static int packsrc_fill_plain(ace_packsrc_t *s)
{
    size_t need, got;
    if (s->hold_off < s->hold_len)
        return ACE_OK;
    s->hold_off = 0;
    s->hold_len = 0;
    if (s->remain == 0)
        return ACE_OK;
    need = sizeof(s->hold);
    if ((uint64_t)need > s->remain)
        need = (size_t)s->remain;
    if (s->encrypted) {
        need &= ~(size_t)7;
        if (need == 0)
            return ACE_ERR_CORRUPT;
    }
    got = segsrc_read(&s->src, s->hold, need);
    if (s->encrypted) {
        if (got % 8)
            return ACE_ERR_CORRUPT;
        ace_bf_decrypt(&s->bf, s->hold, got);
    }
    s->remain -= got;
    s->hold_len = got;
    return ACE_OK;
}

static size_t packsrc_read_exact(void *ctx, void *buf, size_t n)
{
    ace_packsrc_t *s = (ace_packsrc_t *)ctx;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    while (done < n) {
        size_t avail, take;
        if (s->hold_off >= s->hold_len) {
            if (packsrc_fill_plain(s) != ACE_OK)
                break;
            if (s->hold_len == 0)
                break;
        }
        avail = s->hold_len - s->hold_off;
        take = n - done;
        if (take > avail)
            take = avail;
        memcpy(out + done, s->hold + s->hold_off, take);
        s->hold_off += take;
        done += take;
    }
    return done;
}

static size_t packsrc_read(void *ctx, void *buf, size_t n)
{
    ace_packsrc_t *s = (ace_packsrc_t *)ctx;
    uint8_t *out = (uint8_t *)buf;
    size_t done = 0;
    n &= ~(size_t)3;
    if (!s->encrypted)
        return packsrc_plain_read(ctx, buf, n);
    while (done < n) {
        size_t avail, take;
        if (s->hold_off >= s->hold_len) {
            if (packsrc_fill_plain(s) != ACE_OK)
                break;
            if (s->hold_len == 0)
                break;
        }
        avail = s->hold_len - s->hold_off;
        take = n - done;
        if (take > avail)
            take = avail;
        take &= ~(size_t)3;
        if (take == 0) {
            /* keep leftover < 4 until more ciphertext arrives */
            if (s->remain == 0) {
                /* drain remaining 1-3 bytes as zeros by not delivering them */
                break;
            }
            /* need more to align; pull next block */
            if (avail < 4) {
                uint8_t tmp[8];
                size_t keep = avail;
                memcpy(tmp, s->hold + s->hold_off, keep);
                s->hold_off = s->hold_len;
                if (packsrc_fill_plain(s) != ACE_OK)
                    break;
                if (s->hold_len == 0)
                    break;
                if (keep + s->hold_len > sizeof(s->hold))
                    break;
                memmove(s->hold + keep, s->hold, s->hold_len);
                memcpy(s->hold, tmp, keep);
                s->hold_len += keep;
                s->hold_off = 0;
                continue;
            }
            break;
        }
        memcpy(out + done, s->hold + s->hold_off, take);
        s->hold_off += take;
        done += take;
    }
    return done;
}

static int extract_one(ace_archive_t *ar, size_t idx, ace_out_cb cb, void *cb_ctx,
                       const uint8_t *pwd, size_t pwd_len, int restore_solid)
{
    ace_member_t *m;
    ace_crc_out_t wrap;
    ace_filehdr_t *h;
    size_t dicsize;
    int rc;

    if (idx >= ar->nmembers)
        return ACE_ERR_PARAM;
    m = &ar->members[idx];
    h = &m->hdr;

    if (ar->solid && restore_solid && ar->next_read_idx != idx) {
        size_t i;
        size_t start = (ar->next_read_idx < idx) ? ar->next_read_idx : 0;
        if (start == 0 && ar->next_read_idx != 0) {
            ace_engine_free(&ar->engine);
            rc = ace_engine_init(&ar->engine);
            if (rc != ACE_OK)
                return rc;
            ar->next_read_idx = 0;
            start = 0;
        }
        for (i = start; i < idx; i++) {
            rc = ace_archive_test_member(ar, i, pwd, pwd_len);
            if (rc != ACE_OK)
                return rc;
        }
    }

    if ((h->attribs & ACE_ATTR_DIRECTORY) || h->origsize == 0) {
        ar->next_read_idx = idx + 1;
        return ACE_OK;
    }
    if (h->hdr_flags & ACE_FLAG_PASSWORD) {
        if (!pwd || pwd_len == 0)
            return ACE_ERR_PASSWORD;
    }
    if (m->incomplete)
        return ACE_ERR_MULTIVOL;

    ace_crc32_init(&wrap.crc);
    wrap.user = cb;
    wrap.user_ctx = cb_ctx;
    dicsize = (size_t)1 << ((h->params & 15) + 10);

    if (h->comptype == ACE_COMP_STORED) {
        ace_segsrc_t ss;
        ace_packsrc_t psrc;
        uint64_t produced = 0;
        uint8_t blk[131072];
        int enc = (h->hdr_flags & ACE_FLAG_PASSWORD) != 0;
        ss.segs = m->segs;
        ss.nsegs = m->nsegs;
        ss.idx = 0;
        ss.off = 0;
        memset(&psrc, 0, sizeof(psrc));
        if (enc) {
            psrc.src.segs = m->segs;
            psrc.src.nsegs = m->nsegs;
            psrc.remain = h->packsize;
            psrc.encrypted = 1;
            ace_bf_init(&psrc.bf, pwd, pwd_len);
        }
        ace_lz77_setsize(&ar->engine.lz77, dicsize);
        while (produced < h->origsize) {
            size_t want = (size_t)(h->origsize - produced);
            size_t got;
            if (want > sizeof(blk))
                want = sizeof(blk);
            if (enc)
                got = packsrc_read_exact(&psrc, blk, want);
            else
                got = segsrc_read(&ss, blk, want);
            if (got == 0)
                return ACE_ERR_TRUNCATED;
            {
                size_t take = got;
                if (produced + take > h->origsize)
                    take = (size_t)(h->origsize - produced);
                ace_lz77_register(&ar->engine.lz77, blk, take);
                rc = crc_out_cb(&wrap, blk, take);
                if (rc != ACE_OK)
                    return rc;
                produced += take;
            }
        }
    } else {
        ace_bs_t bs;
        ace_packsrc_t src;
        memset(&src, 0, sizeof(src));
        src.src.segs = m->segs;
        src.src.nsegs = m->nsegs;
        src.remain = h->packsize;
        src.encrypted = (h->hdr_flags & ACE_FLAG_PASSWORD) != 0;
        if (src.encrypted) {
            ace_bf_init(&src.bf, pwd, pwd_len);
        }
        rc = ace_bs_init(&bs, packsrc_read, &src, 131072);
        if (rc != ACE_OK)
            return rc;
        if (h->comptype == ACE_COMP_LZ77)
            rc = ace_decompress_lz77(&ar->engine, &bs, h->origsize, dicsize, crc_out_cb, &wrap);
        else if (h->comptype == ACE_COMP_BLOCKED)
            rc = ace_decompress_blocked(&ar->engine, &bs, h->origsize, dicsize, crc_out_cb, &wrap);
        else {
            ace_bs_free(&bs);
            return ACE_ERR_METHOD;
        }
        ace_bs_free(&bs);
        if (rc != ACE_OK)
            return (src.encrypted && (rc == ACE_ERR_CORRUPT || rc == ACE_ERR_EOF)) ? ACE_ERR_PASSWORD : rc;
    }

    if (ace_crc32_final(&wrap.crc) != h->crc32)
        return (h->hdr_flags & ACE_FLAG_PASSWORD) ? ACE_ERR_PASSWORD : ACE_ERR_CRC;
    ar->next_read_idx = idx + 1;
    return ACE_OK;
}

int ace_archive_extract_member(ace_archive_t *ar, size_t idx, ace_out_cb cb, void *cb_ctx,
                               const uint8_t *pwd, size_t pwd_len)
{
    return extract_one(ar, idx, cb, cb_ctx, pwd, pwd_len, 1);
}

int ace_archive_test_member(ace_archive_t *ar, size_t idx, const uint8_t *pwd, size_t pwd_len)
{
    return extract_one(ar, idx, NULL, NULL, pwd, pwd_len, 1);
}

typedef struct {
    FILE *out;
} ace_file_out_t;

static int file_out_cb(void *ctx, const uint8_t *buf, size_t n)
{
    ace_file_out_t *f = (ace_file_out_t *)ctx;
    if (fwrite(buf, 1, n, f->out) != n)
        return ACE_ERR_IO;
    return ACE_OK;
}

static int dos_to_timet(uint32_t dos, time_t *out)
{
    struct tm tmv;
    unsigned year, mon, day, hour, min, sec;
    time_t t;

    if (out)
        *out = 0;
    year = ((dos >> 25) & 0x7F) + 1980;
    mon = (dos >> 21) & 0x0F;
    day = (dos >> 16) & 0x1F;
    hour = (dos >> 11) & 0x1F;
    min = (dos >> 5) & 0x3F;
    sec = (dos & 0x1F) * 2;
    if (mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23 || min > 59 || sec > 61)
        return ACE_ERR_PARAM;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = (int)year - 1900;
    tmv.tm_mon = (int)mon - 1;
    tmv.tm_mday = (int)day;
    tmv.tm_hour = (int)hour;
    tmv.tm_min = (int)min;
    tmv.tm_sec = (int)sec;
    tmv.tm_isdst = -1;
    t = mktime(&tmv);
    if (t == (time_t)-1)
        return ACE_ERR_PARAM;
    if (out)
        *out = t;
    return ACE_OK;
}

static void apply_restore(const char *path, uint32_t datetime, uint32_t attribs, int isdir)
{
    time_t t;
    struct utimbuf ut;

    if (dos_to_timet(datetime, &t) == ACE_OK) {
        ut.actime = t;
        ut.modtime = t;
        utime(path, &ut);
    }
#ifdef _WIN32
    DWORD win_attr = 0;
    if (attribs & ACE_ATTR_READONLY)  win_attr |= FILE_ATTRIBUTE_READONLY;
    if (attribs & ACE_ATTR_HIDDEN)    win_attr |= FILE_ATTRIBUTE_HIDDEN;
    if (attribs & ACE_ATTR_SYSTEM)    win_attr |= FILE_ATTRIBUTE_SYSTEM;
    if (attribs & ACE_ATTR_ARCHIVE)   win_attr |= FILE_ATTRIBUTE_ARCHIVE;
    if (win_attr != 0)
        SetFileAttributesA(path, win_attr);
    (void)isdir;
#else
    mode_t mode = isdir ? 0755 : 0644;
    if (attribs & ACE_ATTR_READONLY)
        mode &= ~(S_IWUSR | S_IWGRP | S_IWOTH);
    chmod(path, mode);
#endif
}

int ace_archive_extract_to_path(ace_archive_t *ar, size_t idx, const char *basedir,
                                const uint8_t *pwd, size_t pwd_len,
                                const ace_extract_opts_t *opts)
{
    ace_member_t *m;
    char path[4096];
    char parent[4096];
    ace_extract_opts_t dflt;
    const char *name;
    ace_file_out_t fo;
    int rc;

    if (idx >= ar->nmembers)
        return ACE_ERR_PARAM;
    m = &ar->members[idx];
    if (!basedir || !basedir[0])
        basedir = ".";
    if (!opts) {
        dflt.junk_paths = 0;
        dflt.restore = 0;
        dflt.unix_paths = 0;
        opts = &dflt;
    }
    if (!ace_is_safe_relpath(m->hdr.filename)) {
        fprintf(stderr, "Security warning: skipping unsafe path traversal '%s'\n", m->hdr.filename);
        return ACE_ERR_PARAM;
    }
    char unbuf[1024];
    name = m->hdr.filename;
    if (opts->unix_paths) {
        ace_format_unix_path(name, unbuf, sizeof(unbuf));
        name = unbuf;
    }
    if (opts->junk_paths) {
        const char *slash = strrchr(name, '/');
        if (slash)
            name = slash + 1;
    }
    if (!name[0] || strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return ACE_ERR_PARAM;
    snprintf(path, sizeof(path), "%s/%s", basedir, name);

    if (m->hdr.attribs & ACE_ATTR_DIRECTORY) {
        rc = ace_mkdir_p(path);
        if (rc == ACE_OK && opts->restore)
            apply_restore(path, m->hdr.datetime, m->hdr.attribs, 1);
        return rc;
    }

    snprintf(parent, sizeof(parent), "%s", path);
    {
        char *slash = strrchr(parent, '/');
        if (slash && slash != parent) {
            *slash = 0;
            rc = ace_mkdir_p(parent);
            if (rc != ACE_OK)
                return rc;
        }
    }
    fo.out = fopen(path, "wb");
    if (!fo.out)
        return ACE_ERR_IO;
    rc = ace_archive_extract_member(ar, idx, file_out_cb, &fo, pwd, pwd_len);
    fclose(fo.out);
    if (rc == ACE_OK && opts->restore)
        apply_restore(path, m->hdr.datetime, m->hdr.attribs, 0);
    return rc;
}
