#ifndef ACE_ARCHIVE_H
#define ACE_ARCHIVE_H

#include "ace/engine.h"
#include "ace/crc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ACE_MAGIC "**ACE**"

#define ACE_TYPE_MAIN        0
#define ACE_TYPE_FILE32      1
#define ACE_TYPE_RECOVERY32  2
#define ACE_TYPE_FILE64      3
#define ACE_TYPE_RECOVERY64A 4
#define ACE_TYPE_RECOVERY64B 5

#define ACE_HOST_MSDOS       0
#define ACE_HOST_OS2         1
#define ACE_HOST_WIN32       2
#define ACE_HOST_UNIX        3
#define ACE_HOST_LINUX       12

#define ACE_FLAG_ADDSIZE     (1u << 0)
#define ACE_FLAG_COMMENT     (1u << 1)
#define ACE_FLAG_64BIT       (1u << 2)
#define ACE_FLAG_V20FORMAT   (1u << 8)
#define ACE_FLAG_SFX         (1u << 9)
#define ACE_FLAG_LIMITSFXJR  (1u << 10)
#define ACE_FLAG_NTSECURITY  (1u << 10)
#define ACE_FLAG_MULTIVOLUME (1u << 11)
#define ACE_FLAG_ADVERT      (1u << 12)
#define ACE_FLAG_CONTPREV    (1u << 12)
#define ACE_FLAG_RECOVERY    (1u << 13)
#define ACE_FLAG_CONTNEXT    (1u << 13)
#define ACE_FLAG_LOCKED      (1u << 14)
#define ACE_FLAG_PASSWORD    (1u << 14)
#define ACE_FLAG_SOLID       (1u << 15)

#define ACE_ATTR_READONLY    0x00000001u
#define ACE_ATTR_HIDDEN      0x00000002u
#define ACE_ATTR_SYSTEM      0x00000004u
#define ACE_ATTR_VOLUME      0x00000008u
#define ACE_ATTR_DIRECTORY   0x00000010u
#define ACE_ATTR_ARCHIVE     0x00000020u

#define ACE_QUAL_NONE    0
#define ACE_QUAL_FASTEST 1
#define ACE_QUAL_FAST    2
#define ACE_QUAL_NORMAL  3
#define ACE_QUAL_GOOD    4
#define ACE_QUAL_BEST    5

typedef struct {
    uint16_t hdr_crc;
    uint16_t hdr_size;
    uint8_t hdr_type;
    uint16_t hdr_flags;
    uint64_t packsize;
    uint64_t origsize;
    uint32_t datetime;
    uint32_t attribs;
    uint32_t crc32;
    uint8_t comptype;
    uint8_t compqual;
    uint16_t params;
    uint16_t reserved1;
    char filename[1024];
    uint8_t raw_filename[1024];
    size_t raw_filename_len;
    uint8_t *comment;
    size_t comment_len;
    uint8_t *ntsecurity;
    size_t ntsecurity_len;
    uint64_t dataoffset;
} ace_filehdr_t;

typedef struct {
    uint16_t hdr_crc;
    uint16_t hdr_size;
    uint8_t hdr_type;
    uint16_t hdr_flags;
    uint8_t magic[7];
    uint8_t eversion;
    uint8_t cversion;
    uint8_t host;
    uint8_t volume;
    uint32_t datetime;
    uint8_t reserved1[8];
    uint8_t *advert;
    size_t advert_len;
    uint8_t *comment;
    size_t comment_len;
} ace_mainhdr_t;

typedef struct {
    FILE *fp;
    uint64_t offset;
    uint64_t size;
} ace_seg_t;

typedef struct {
    ace_filehdr_t hdr;
    FILE *fp;
    ace_seg_t *segs;
    size_t nsegs;
    int incomplete;
} ace_member_t;

typedef struct {
    char path[4096];
    FILE *fp;
    uint64_t filesize;
    uint8_t volume;
} ace_vol_t;

typedef struct {
    char path[4096];
    FILE *fp;
    FILE *cur_fp;
    uint64_t filesize;
    ace_mainhdr_t main;
    ace_vol_t *vols;
    size_t nvols;
    size_t vols_cap;
    ace_member_t *members;
    size_t nmembers;
    size_t members_cap;
    int solid;
    int multivolume;
    int locked;
    ace_engine_t engine;
    int engine_ready;
    size_t next_read_idx;
} ace_archive_t;

int ace_archive_open(ace_archive_t *ar, const char *path, size_t search);
void ace_archive_close(ace_archive_t *ar);

int ace_archive_extract_member(ace_archive_t *ar, size_t idx, ace_out_cb cb, void *cb_ctx,
                               const uint8_t *pwd, size_t pwd_len);
int ace_archive_test_member(ace_archive_t *ar, size_t idx, const uint8_t *pwd, size_t pwd_len);

typedef struct {
    int junk_paths;  /* 1 = store basename only, drop directories from the member path */
    int restore;     /* 1 = restore modification time and file attributes */
    int unix_paths;  /* 1 = format paths as Unix relative paths (/ slashes, lowercase) */
} ace_extract_opts_t;

int ace_archive_extract_to_path(ace_archive_t *ar, size_t idx, const char *basedir,
                                const uint8_t *pwd, size_t pwd_len,
                                const ace_extract_opts_t *opts);

const char *ace_host_str(unsigned host);
const char *ace_comp_str(unsigned t);
const char *ace_qual_str(unsigned q);

#ifdef __cplusplus
}
#endif

#endif
