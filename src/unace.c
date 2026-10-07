#include "ace/archive.h"
#include "ace/cli.h"
#include "ace/oem.h"
#include "ace/util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "%s"
            "Usage: %s [-l|-t|-x|-H] [-d DIR] [-p PASS] [-s] [-j] [-k] [-1|--bare] [-u|--unix] ARCHIVE [PAT...]\n"
            "  -l  list archive contents (default)\n"
            "  -1, --bare  bare listing (one path per line, script/pipe friendly)\n"
            "  -u, --unix  format paths as Unix relative paths (/ slashes, lowercase)\n"
            "  -t  test archive integrity\n"
            "  -x  extract files\n"
            "  -H  dump headers\n"
            "  -d  extraction directory\n"
            "  -p  password\n"
            "  -s  search 512KiB for SFX stub\n"
            "  -j  junk (strip) stored directory names on extract\n"
            "  -k  keep/restore file modification time and attributes\n"
            "  --oem CP  decode member names from OEM code page 850 or 437\n"
            "  -v  verbose\n"
            "  PAT member name or wildcard pattern (case-insensitive)\n",
            ACE_BANNER, argv0);
}

static int member_matches(const char *pattern, const char *name)
{
    const char *base;
    if (ace_wildcard_match(pattern, name, 1))
        return 1;
    base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (base != name && ace_wildcard_match(pattern, base, 1))
        return 1;
    return 0;
}

static int matches_any_pattern(const char *name, const char **patterns, int npatterns)
{
    if (npatterns == 0)
        return 1;
    for (int j = 0; j < npatterns; j++) {
        if (member_matches(patterns[j], name))
            return 1;
    }
    return 0;
}

static void print_dos_time(uint32_t dos, char *buf, size_t n)
{
    unsigned year = ((dos >> 25) & 0x7F) + 1980;
    unsigned mon = (dos >> 21) & 0x0F;
    unsigned day = (dos >> 16) & 0x1F;
    unsigned hour = (dos >> 11) & 0x1F;
    unsigned min = (dos >> 5) & 0x3F;
    unsigned sec = (dos & 0x1F) * 2;
    if (mon < 1 || mon > 12 || day < 1 || day > 31) {
        snprintf(buf, n, "1980-01-01 00:00:00");
        return;
    }
    snprintf(buf, n, "%04u-%02u-%02u %02u:%02u:%02u", year, mon, day, hour, min, sec);
}

int unace_main(int argc, char **argv)
{
    const char *mode = "list";
    const char *basedir = ".";
    const char *password = NULL;
    const char *archive = NULL;
    int verbose = 0;
    int junk = 0;
    int keep = 0;
    int bare = 0;
    int unix_paths = 0;
    const char *patterns[64];
    int npatterns = 0;
    ace_oem_t oem = ACE_OEM_NONE;
    size_t search = 524288;
    ace_archive_t ar;
    int rc;
    int i;
    size_t k;

    for (i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if (strcmp(argv[i], "-l") == 0)
                mode = "list";
            else if (strcmp(argv[i], "-t") == 0)
                mode = "test";
            else if (strcmp(argv[i], "-x") == 0)
                mode = "extract";
            else if (strcmp(argv[i], "-H") == 0)
                mode = "headers";
            else if (strcmp(argv[i], "-v") == 0)
                verbose = 1;
            else if (strcmp(argv[i], "-j") == 0)
                junk = 1;
            else if (strcmp(argv[i], "-k") == 0)
                keep = 1;
            else if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--bare") == 0 || strcmp(argv[i], "-b") == 0)
                bare = 1;
            else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--unix") == 0)
                unix_paths = 1;
            else if (strcmp(argv[i], "-1u") == 0 || strcmp(argv[i], "-u1") == 0) {
                bare = 1;
                unix_paths = 1;
            } else if (strcmp(argv[i], "--oem") == 0 && i + 1 < argc)
                oem = ace_oem_parse(argv[++i]);
            else if (strcmp(argv[i], "-s") == 0)
                search = 524288;
            else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc)
                basedir = argv[++i];
            else if (strncmp(argv[i], "-d", 2) == 0 && argv[i][2] != '\0')
                basedir = argv[i] + 2;
            else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc)
                password = argv[++i];
            else if (strncmp(argv[i], "-p", 2) == 0 && argv[i][2] != '\0')
                password = argv[i] + 2;
            else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
                usage(argv[0]);
                return 0;
            } else {
                usage(argv[0]);
                return 1;
            }
        } else {
            if (!archive)
                archive = argv[i];
            else if (npatterns < 64)
                patterns[npatterns++] = argv[i];
        }
    }
    if (!archive) {
        usage(argv[0]);
        return 1;
    }

    rc = ace_archive_open(&ar, archive, search);
    if (rc != ACE_OK) {
        fprintf(stderr, "%s: %s\n", archive, ace_strerror(rc));
        return 1;
    }

    if (oem != ACE_OEM_NONE) {
        char uftmp[1024];
        for (k = 0; k < ar.nmembers; k++) {
            ace_filehdr_t *h = &ar.members[k].hdr;
            ace_oem_to_utf8(h->raw_filename, h->raw_filename_len, oem, uftmp, sizeof(uftmp));
            ace_sanitize_path((const uint8_t *)uftmp, strlen(uftmp),
                              h->filename, sizeof(h->filename));
            if (h->filename[0] == 0)
                snprintf(h->filename, sizeof(h->filename), "file%04zu", k);
        }
    }

    if (verbose) {
        char ts[32];
        print_dos_time(ar.main.datetime, ts, sizeof(ts));
        printf("archive %s  ACE %u.%u  host %s  %s%s%s  %s\n",
               archive,
               ar.main.cversion / 10, ar.main.cversion % 10,
               ace_host_str(ar.main.host),
               ar.solid ? "solid " : "",
               ar.multivolume ? "multivolume " : "",
               ar.locked ? "locked" : "",
               ts);
        if (ar.main.advert_len)
            printf("advert: %.*s\n", (int)ar.main.advert_len, ar.main.advert);
        if (ar.main.comment_len)
            printf("comment: %.*s\n", (int)ar.main.comment_len, ar.main.comment);
    }

    if (strcmp(mode, "headers") == 0) {
        char ts[32];
        print_dos_time(ar.main.datetime, ts, sizeof(ts));
        printf("MAIN crc=%04x size=%u flags=%04x e=%u c=%u host=%s vol=%u %s\n",
               ar.main.hdr_crc, ar.main.hdr_size, ar.main.hdr_flags,
               ar.main.eversion, ar.main.cversion, ace_host_str(ar.main.host),
               ar.main.volume, ts);
        for (k = 0; k < ar.nmembers; k++) {
            ace_filehdr_t *h = &ar.members[k].hdr;
            if (!matches_any_pattern(h->filename, patterns, npatterns))
                continue;
            char disp[1024];
            if (unix_paths)
                ace_format_unix_path(h->filename, disp, sizeof(disp));
            else
                snprintf(disp, sizeof(disp), "%s", h->filename);
            print_dos_time(h->datetime, ts, sizeof(ts));
            printf("FILE %-40s pack=%llu orig=%llu crc=%08x %s/%s flags=%04x attr=%08x cm=%zu ntsec=%zu %s\n",
                   disp,
                   (unsigned long long)h->packsize,
                   (unsigned long long)h->origsize,
                   h->crc32,
                   ace_comp_str(h->comptype),
                   ace_qual_str(h->compqual),
                   h->hdr_flags, h->attribs,
                   h->comment_len, h->ntsecurity_len, ts);
        }
        ace_archive_close(&ar);
        return 0;
    }

    if (strcmp(mode, "list") == 0) {
        if (bare) {
            for (k = 0; k < ar.nmembers; k++) {
                ace_filehdr_t *h = &ar.members[k].hdr;
                if (!matches_any_pattern(h->filename, patterns, npatterns))
                    continue;
                char disp[1024];
                if (unix_paths)
                    ace_format_unix_path(h->filename, disp, sizeof(disp));
                else
                    snprintf(disp, sizeof(disp), "%s", h->filename);
                printf("%s\n", disp);
            }
            ace_archive_close(&ar);
            return 0;
        }

        size_t shown = 0;
        printf("%-10s %10s %10s %-9s %s\n", "Date", "Packed", "Size", "Method", "Name");
        for (k = 0; k < ar.nmembers; k++) {
            ace_filehdr_t *h = &ar.members[k].hdr;
            char ts[32];
            if (!matches_any_pattern(h->filename, patterns, npatterns))
                continue;
            shown++;
            print_dos_time(h->datetime, ts, sizeof(ts));
            char disp[1024];
            if (unix_paths)
                ace_format_unix_path(h->filename, disp, sizeof(disp));
            else
                snprintf(disp, sizeof(disp), "%s", h->filename);
            printf("%s %10llu %10llu %-9s %s%s\n",
                   ts,
                   (unsigned long long)h->packsize,
                   (unsigned long long)h->origsize,
                   ace_comp_str(h->comptype),
                   disp,
                   (h->hdr_flags & ACE_FLAG_PASSWORD) ? " [enc]" : "");
        }
        printf("%zu file(s)\n", shown);
        ace_archive_close(&ar);
        return 0;
    }

    {
        const uint8_t *pwd = password ? (const uint8_t *)password : NULL;
        size_t pwd_len = password ? strlen(password) : 0;
        ace_extract_opts_t opts;
        int failed = 0;
        opts.junk_paths = junk;
        opts.restore = keep;
        opts.unix_paths = unix_paths;
        for (k = 0; k < ar.nmembers; k++) {
            ace_filehdr_t *h = &ar.members[k].hdr;
            if (!matches_any_pattern(h->filename, patterns, npatterns))
                continue;
            char disp[1024];
            if (unix_paths)
                ace_format_unix_path(h->filename, disp, sizeof(disp));
            else
                snprintf(disp, sizeof(disp), "%s", h->filename);
            if (strcmp(mode, "test") == 0) {
                rc = ace_archive_test_member(&ar, k, pwd, pwd_len);
                printf("%s: %s\n", disp, rc == ACE_OK ? "OK" : ace_strerror(rc));
                if (rc != ACE_OK)
                    failed++;
            } else {
                if (!ace_is_safe_relpath(h->filename)) {
                    fprintf(stderr, "Security warning: skipping unsafe path traversal '%s'\n", h->filename);
                    failed++;
                    continue;
                }
                rc = ace_archive_extract_to_path(&ar, k, basedir, pwd, pwd_len, &opts);
                if (verbose || rc != ACE_OK)
                    printf("%s: %s\n", disp, rc == ACE_OK ? "extracted" : ace_strerror(rc));
                if (rc != ACE_OK)
                    failed++;
            }
        }
        ace_archive_close(&ar);
        return failed ? 1 : 0;
    }
}
