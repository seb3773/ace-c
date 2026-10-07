/*
 * sfx_linux_stub.c - Lightweight standalone Linux ELF SFX extractor stub for ACE archives.
 *
 * Provides autonomous self-extraction, listing, testing, and pattern filtering
 * for ACE self-extracting archives on Linux x86_64 without external dependencies.
 */
#define _GNU_SOURCE
#define _DEFAULT_SOURCE

#include "ace/archive.h"
#include "ace/cli.h"
#include "ace/oem.h"
#include "ace/crc.h"
#include "ace/util.h"

#include <ctype.h>
#include <fnmatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static int member_matches(const char *pat, const char *name)
{
    if (!pat || !pat[0])
        return 1;
    if (fnmatch(pat, name, FNM_CASEFOLD) == 0)
        return 1;
    const char *base = strrchr(name, '/');
    base = base ? base + 1 : name;
    if (base != name && fnmatch(pat, base, FNM_CASEFOLD) == 0)
        return 1;
    return 0;
}

static int matches_any(const char *name, const char **patterns, int n_pats)
{
    if (n_pats <= 0)
        return 1;
    for (int i = 0; i < n_pats; i++) {
        if (member_matches(patterns[i], name))
            return 1;
    }
    return 0;
}

static void format_dos_date(uint32_t dos, char *buf, size_t n)
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

static void show_help(const char *prog)
{
    printf("ACE Linux SFX v2.0 - Self-Extracting Archive (x86_64 ELF)\n\n");
    printf("Usage: %s [OPTIONS] [PATTERNS...]\n\n", prog);
    printf("Options:\n");
    printf("  -x, --extract   Extract files with full pathnames (default)\n");
    printf("  -e              Extract files flat without paths (junk paths)\n");
    printf("  -d <dir>        Extract into destination directory\n");
    printf("  -l, --list      List archive contents\n");
    printf("  -1, --bare      Bare listing (one path per line, script/pipe friendly)\n");
    printf("  -u, --unix      Format paths as Unix relative paths (/ slashes, lowercase)\n");
    printf("  -v              Verbose list\n");
    printf("  -t, --test      Test archive integrity (verify CRC)\n");
    printf("  -p <pass>       Password for encrypted members\n");
    printf("  -k              Restore original file timestamps\n");
    printf("  -j              Strip directory paths on extract\n");
    printf("  -h, --help      Show this help\n\n");
    printf("Examples:\n");
    printf("  %s                      # extract all files to current directory\n", prog);
    printf("  %s -d /tmp/output       # extract all files to /tmp/output\n", prog);
    printf("  %s -l \"*.txt\"           # list text files\n", prog);
    printf("  %s -l -1                # bare list of member paths\n", prog);
    printf("  %s -t                   # verify archive integrity\n", prog);
}

int main(int argc, char **argv)
{
    const char *basedir = ".";
    const char *password = NULL;
    int mode = 0; /* 0 = extract, 1 = list, 2 = test */
    int junk = 0;
    int keep = 0;
    int verbose = 0;
    int bare = 0;
    int unix_paths = 0;
    const char *patterns[64];
    int n_patterns = 0;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            show_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--list") == 0) {
            mode = 1;
        } else if (strcmp(argv[i], "-v") == 0) {
            mode = 1;
            verbose = 1;
        } else if (strcmp(argv[i], "-1") == 0 || strcmp(argv[i], "--bare") == 0 || strcmp(argv[i], "-b") == 0) {
            bare = 1;
            mode = 1;
        } else if (strcmp(argv[i], "-u") == 0 || strcmp(argv[i], "--unix") == 0) {
            unix_paths = 1;
        } else if (strcmp(argv[i], "-1u") == 0 || strcmp(argv[i], "-u1") == 0) {
            bare = 1;
            unix_paths = 1;
            mode = 1;
        } else if (strcmp(argv[i], "-t") == 0 || strcmp(argv[i], "--test") == 0) {
            mode = 2;
        } else if (strcmp(argv[i], "-x") == 0 || strcmp(argv[i], "--extract") == 0) {
            mode = 0;
        } else if (strcmp(argv[i], "-e") == 0) {
            mode = 0;
            junk = 1;
        } else if (strcmp(argv[i], "-j") == 0) {
            junk = 1;
        } else if (strcmp(argv[i], "-k") == 0) {
            keep = 1;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            basedir = argv[++i];
        } else if (strncmp(argv[i], "-d", 2) == 0 && argv[i][2] != '\0') {
            basedir = argv[i] + 2;
        } else if (strcmp(argv[i], "-p") == 0 && i + 1 < argc) {
            password = argv[++i];
        } else if (strncmp(argv[i], "-p", 2) == 0 && argv[i][2] != '\0') {
            password = argv[i] + 2;
        } else if (argv[i][0] != '-') {
            if (n_patterns < 64)
                patterns[n_patterns++] = argv[i];
        } else {
            fprintf(stderr, "Unknown option: %s (try -h for help)\n", argv[i]);
            return 1;
        }
    }

    const char *self = "/proc/self/exe";
    if (access(self, R_OK) != 0 && argc > 0 && argv[0])
        self = argv[0];

    ace_archive_t ar;
    int rc = ace_archive_open(&ar, self, 1024 * 1024);
    if (rc != ACE_OK) {
        fprintf(stderr, "Error: unable to locate embedded ACE archive in '%s': %s\n",
                self, ace_strerror(rc));
        return 1;
    }

    size_t pwd_len = password ? strlen(password) : 0;
    const uint8_t *pwd = (const uint8_t *)password;
    int errors = 0;

    if (mode == 1) {
        /* List mode */
        if (bare) {
            for (size_t i = 0; i < ar.nmembers; i++) {
                ace_member_t *m = &ar.members[i];
                if (!matches_any(m->hdr.filename, patterns, n_patterns))
                    continue;
                char disp[1024];
                if (unix_paths)
                    ace_format_unix_path(m->hdr.filename, disp, sizeof(disp));
                else
                    snprintf(disp, sizeof(disp), "%s", m->hdr.filename);
                printf("%s\n", disp);
            }
            ace_archive_close(&ar);
            return 0;
        }

        printf("ACE Self-Extracting Archive v2.0 (Linux x86_64)\n");
        if (ar.main.comment && ar.main.comment_len > 0)
            printf("Main comment: %.*s\n", (int)ar.main.comment_len, (char *)ar.main.comment);
        printf("\nDate           Time       Packed       Size Method    Name\n");
        printf("-----------------------------------------------------------\n");

        uint64_t tot_pack = 0, tot_orig = 0;
        size_t matched_count = 0;
        char dtbuf[32];

        (void)verbose;
        for (size_t i = 0; i < ar.nmembers; i++) {
            ace_member_t *m = &ar.members[i];
            if (!matches_any(m->hdr.filename, patterns, n_patterns))
                continue;
            matched_count++;
            format_dos_date(m->hdr.datetime, dtbuf, sizeof(dtbuf));
            tot_pack += m->hdr.packsize;
            tot_orig += m->hdr.origsize;
            char disp[1024];
            if (unix_paths)
                ace_format_unix_path(m->hdr.filename, disp, sizeof(disp));
            else
                snprintf(disp, sizeof(disp), "%s", m->hdr.filename);
            printf("%s %10llu %10llu %-9s %s\n",
                   dtbuf,
                   (unsigned long long)m->hdr.packsize,
                   (unsigned long long)m->hdr.origsize,
                   ace_comp_str(m->hdr.comptype),
                   disp);
        }
        printf("-----------------------------------------------------------\n");
        printf("%zu file(s), %llu bytes uncompressed (%llu bytes packed)\n",
               matched_count, (unsigned long long)tot_orig, (unsigned long long)tot_pack);
    } else if (mode == 2) {
        /* Test mode */
        printf("Testing archive integrity...\n");
        size_t tested = 0;
        for (size_t i = 0; i < ar.nmembers; i++) {
            ace_member_t *m = &ar.members[i];
            if (!matches_any(m->hdr.filename, patterns, n_patterns))
                continue;
            tested++;
            char disp[1024];
            if (unix_paths)
                ace_format_unix_path(m->hdr.filename, disp, sizeof(disp));
            else
                snprintf(disp, sizeof(disp), "%s", m->hdr.filename);
            printf("  %-45s ", disp);
            fflush(stdout);
            rc = ace_archive_test_member(&ar, i, pwd, pwd_len);
            if (rc == ACE_OK) {
                printf("OK\n");
            } else {
                printf("FAILED (%s)\n", ace_strerror(rc));
                errors++;
            }
        }
        if (errors == 0)
            printf("All %zu tested files OK.\n", tested);
        else
            printf("%d file(s) failed integrity check!\n", errors);
    } else {
        /* Extract mode */
        printf("Extracting archive...\n");
        ace_extract_opts_t opts;
        opts.junk_paths = junk;
        opts.restore = keep;
        opts.unix_paths = unix_paths;

        size_t extracted = 0;
        for (size_t i = 0; i < ar.nmembers; i++) {
            ace_member_t *m = &ar.members[i];
            if (!matches_any(m->hdr.filename, patterns, n_patterns))
                continue;
            if (!ace_is_safe_relpath(m->hdr.filename)) {
                fprintf(stderr, "Security warning: skipping unsafe path traversal '%s'\n", m->hdr.filename);
                errors++;
                continue;
            }
            extracted++;
            char disp[1024];
            if (unix_paths)
                ace_format_unix_path(m->hdr.filename, disp, sizeof(disp));
            else
                snprintf(disp, sizeof(disp), "%s", m->hdr.filename);
            printf("  extracting %-40s ", disp);
            fflush(stdout);
            rc = ace_archive_extract_to_path(&ar, i, basedir, pwd, pwd_len, &opts);
            if (rc == ACE_OK) {
                printf("OK\n");
            } else {
                printf("FAILED (%s)\n", ace_strerror(rc));
                errors++;
            }
        }
        printf("Extracted %zu file(s)%s.\n", extracted, errors ? " (with errors)" : " successfully");
    }

    ace_archive_close(&ar);
    return errors ? 1 : 0;
}
