/*
 * ace - unified ACE 1.0/2.x compressor/decompressor front-end.
 *
 * One binary, DOS-ACE-style subcommands:
 *   ace a ARCHIVE [opts] FILE...   create archive (opts: -0 -z -2 -s -m -sfx ...)
 *   ace s [opts] ARCHIVE [OUT]     convert archive to SFX executable
 *   ace x [opts] ARCHIVE [PAT...]  extract with full paths (opts: -d -p -k -j ...)
 *   ace e [opts] ARCHIVE [PAT...]  extract without paths (junk paths)
 *   ace l [opts] ARCHIVE [PAT...]  list archive contents
 *   ace v [opts] ARCHIVE [PAT...]  verbose list archive contents
 *   ace t [opts] ARCHIVE [PAT...]  test archive integrity
 *   ace d [opts] ARCHIVE           dump raw archive headers
 *   ace h                          show help
 */
#include "ace.h"
#include "ace/cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
            "%s"
            "Usage: %s COMMAND [OPTIONS] ARCHIVE [FILES... / PATTERNS...]\n\n"
            "Commands:\n"
            "  a ARCHIVE [opts] FILE...  create archive\n"
            "                            options: -0 (store), -z (lz77), -2 (blocked 2.0),\n"
            "                                     -s (solid), -m 0..5 (quality), -d KB (dic),\n"
            "                                     -s8/-s16/-s32a/-s32b (sound), -xe (exe),\n"
            "                                     -dl (delta), -p W[:P] (pic),\n"
            "                                     -sfx[=TYPE] (self-extracting: dos, win32, gui, linux),\n"
            "                                     -pw PASS, -cm TEXT, -cf TEXT, -V BYTES,\n"
            "                                     -A (full paths), -k (lock), -x PAT, @LIST\n"
            "  s [opts] ARCHIVE [OUT]    convert archive to SFX (.exe/.sfx)\n"
            "                            options: -sfx[=TYPE] (types: dos, win32, gui, linux)\n"
            "  x [opts] ARCHIVE [PAT...] extract with full pathnames\n"
            "                            options: -d DIR, -p PASS, -k, -j, -u (--unix), --oem CP, -v\n"
            "  e [opts] ARCHIVE [PAT...] extract without pathnames (junk paths)\n"
            "  l [opts] ARCHIVE [PAT...] list archive contents\n"
            "                            options: -1 (--bare), -u (--unix), -v\n"
            "  v [opts] ARCHIVE [PAT...] verbose list archive contents\n"
            "                            options: -1 (--bare), -u (--unix)\n"
            "  t [opts] ARCHIVE [PAT...] test archive integrity\n"
            "  d [opts] ARCHIVE          dump raw archive headers\n"
            "  h, --help                 show this help\n",
            ACE_BANNER, argv0);
}

static const char *base_name(const char *p)
{
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

static int is_cmd(const char *s, const char *short_, const char *long_)
{
    return strcmp(s, short_) == 0 || (long_ && strcmp(s, long_) == 0);
}

/* Run fn with argv = {tag, lead1..., lead2..., rest of argv from index `from`}. */
static int run_rebuilt(const char *tag, const char *lead1, const char *lead2,
                       int argc, char **argv, int from,
                       int (*fn)(int, char **))
{
    int lead_n = 1 + (lead1 != NULL) + (lead2 != NULL);
    int rest = argc - from;
    int n = lead_n + rest;
    char **av = malloc((size_t)(n + 1) * sizeof(*av));
    int i;
    int rc;

    if (!av) {
        fprintf(stderr, "ace: out of memory\n");
        return 2;
    }
    av[0] = (char *)tag;
    i = 1;
    if (lead1)
        av[i++] = (char *)lead1;
    if (lead2)
        av[i++] = (char *)lead2;
    for (; from < argc; from++)
        av[i++] = argv[from];
    av[i] = NULL;
    rc = fn(n, av);
    free(av);
    return rc;
}

/* Dispatch archive creation ("a" command).
 * Supports:
 *   ace a ARCHIVE [opts] FILE...
 *   ace a [opts] ARCHIVE FILE...
 *   ace a -o ARCHIVE [opts] FILE...
 */
static int dispatch_add(int argc, char **argv)
{
    int i, arch_idx = -1;
    int has_o = 0;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0) {
            has_o = 1;
            break;
        }
    }

    if (has_o) {
        return run_rebuilt("ace", NULL, NULL, argc, argv, 2, mkace_main);
    }

    /* Find the archive name: first non-option positional argument */
    for (i = 2; i < argc; i++) {
        if (argv[i][0] == '-') {
            /* Skip parameter arguments for options that take values */
            if (strcmp(argv[i], "-m") == 0 ||
                strcmp(argv[i], "-d") == 0 ||
                strcmp(argv[i], "-V") == 0 ||
                strcmp(argv[i], "-pw") == 0 ||
                strcmp(argv[i], "-cm") == 0 ||
                strcmp(argv[i], "-cf") == 0 ||
                strcmp(argv[i], "-p") == 0 ||
                strcmp(argv[i], "-x") == 0) {
                i++; /* skip value */
            } else if (strcmp(argv[i], "-sfx") == 0) {
                if (i + 1 < argc && argv[i + 1][0] != '-' &&
                    ace_sfx_parse_type(argv[i + 1]) != ACE_SFX_NONE) {
                    i++; /* skip type value */
                }
            }
            continue;
        }
        arch_idx = i;
        break;
    }

    if (arch_idx < 0) {
        fprintf(stderr, "ace: 'a' requires an archive and at least one file or listfile\n");
        return 2;
    }

    /* Rebuild argv for mkace_main: "ace", "-o", archive, <all other args> */
    int n = argc + 2;
    char **av = malloc((size_t)n * sizeof(*av));
    if (!av) {
        fprintf(stderr, "ace: out of memory\n");
        return 2;
    }

    int cur = 0;
    av[cur++] = "ace";
    av[cur++] = "-o";
    av[cur++] = argv[arch_idx];
    for (i = 2; i < argc; i++) {
        if (i == arch_idx)
            continue;
        av[cur++] = argv[i];
    }
    av[cur] = NULL;
    int rc = mkace_main(cur, av);
    free(av);
    return rc;
}

/* Dispatch SFX conversion ("s" command).
 * Supports:
 *   ace s ARCHIVE [OUT.EXE]
 *   ace s -sfx[=TYPE] ARCHIVE [OUT.EXE]
 */
static int dispatch_sfx(int argc, char **argv)
{
    const char *in_path = NULL;
    const char *out_path = NULL;
    ace_sfx_type_t type = ACE_SFX_DEFAULT_TYPE;
    int i;

    for (i = 2; i < argc; i++) {
        if (strcmp(argv[i], "-sfx-") == 0) {
            type = ACE_SFX_NONE;
        } else if (strcmp(argv[i], "-sfx") == 0) {
            if (i + 1 < argc && argv[i + 1][0] != '-') {
                ace_sfx_type_t t = ace_sfx_parse_type(argv[i + 1]);
                if (t != ACE_SFX_NONE) {
                    type = t;
                    i++;
                } else {
                    type = ACE_SFX_DEFAULT_TYPE;
                }
            } else {
                type = ACE_SFX_DEFAULT_TYPE;
            }
        } else if (strncmp(argv[i], "-sfx=", 5) == 0) {
            type = ace_sfx_parse_type(argv[i] + 5);
            if (type == ACE_SFX_NONE)
                type = ACE_SFX_DEFAULT_TYPE;
        } else if (strncmp(argv[i], "-sfx", 4) == 0 && argv[i][4] != '\0') {
            type = ace_sfx_parse_type(argv[i] + 4);
            if (type == ACE_SFX_NONE)
                type = ACE_SFX_DEFAULT_TYPE;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "ace: unknown option '%s' for 's' command\n", argv[i]);
            return 2;
        } else {
            if (!in_path)
                in_path = argv[i];
            else if (!out_path)
                out_path = argv[i];
            else {
                fprintf(stderr, "ace: unexpected extra argument '%s'\n", argv[i]);
                return 2;
            }
        }
    }

    if (!in_path) {
        fprintf(stderr, "ace: 's' requires an archive to convert (try: ace s [opts] ARCHIVE [OUT.EXE])\n");
        return 2;
    }

    char default_out[4096];
    const char *target = out_path;
    if (!target) {
        size_t len = strlen(in_path);
        if (len >= sizeof(default_out))
            len = sizeof(default_out) - 5;
        memcpy(default_out, in_path, len);
        default_out[len] = '\0';
        const char *ext = (type == ACE_SFX_LINUX) ? ".sfx" : ".exe";
        if (len > 4 && strcasecmp(default_out + len - 4, ".ace") == 0) {
            memcpy(default_out + len - 4, ext, 4);
        } else {
            strncat(default_out, ext, sizeof(default_out) - len - 1);
        }
        target = default_out;
    }

    printf("Converting '%s' to SFX '%s' (%s stub)...\n", in_path, target, ace_sfx_type_name(type));
    int rc = ace_sfx_convert_archive(in_path, target, type);
    if (rc == ACE_OK) {
        printf("SFX archive created successfully: %s\n", target);
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    const char *name = base_name(argv[0]);

    /* Backward compatibility fallback if invoked as mkace or unace */
    if (strcmp(name, "mkace") == 0 || strcmp(name, "mkace.exe") == 0)
        return mkace_main(argc, argv);
    if (strcmp(name, "unace") == 0 || strcmp(name, "unace.exe") == 0)
        return unace_main(argc, argv);

    if (argc < 2) {
        usage(argv[0]);
        return 2;
    }

    if (is_cmd(argv[1], "h", "help") || is_cmd(argv[1], "-?", "--help")) {
        usage(argv[0]);
        return 0;
    }
    if (is_cmd(argv[1], "a", "add"))
        return dispatch_add(argc, argv);
    if (is_cmd(argv[1], "s", "sfx"))
        return dispatch_sfx(argc, argv);
    if (is_cmd(argv[1], "x", "extract"))
        return run_rebuilt("ace", "-x", NULL, argc, argv, 2, unace_main);
    if (is_cmd(argv[1], "e", "extract-flat"))
        return run_rebuilt("ace", "-x", "-j", argc, argv, 2, unace_main);
    if (is_cmd(argv[1], "l", "list"))
        return run_rebuilt("ace", "-l", NULL, argc, argv, 2, unace_main);
    if (is_cmd(argv[1], "v", "verbose-list"))
        return run_rebuilt("ace", "-l", "-v", argc, argv, 2, unace_main);
    if (is_cmd(argv[1], "t", "test"))
        return run_rebuilt("ace", "-t", NULL, argc, argv, 2, unace_main);
    if (is_cmd(argv[1], "d", "dump"))
        return run_rebuilt("ace", "-H", NULL, argc, argv, 2, unace_main);

    fprintf(stderr, "ace: unknown command '%s' (try: ace h)\n", argv[1]);
    return 2;
}
