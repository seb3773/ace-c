#ifndef ACE_OEM_H
#define ACE_OEM_H

#include "ace/util.h"

#ifdef __cplusplus
extern "C" {
#endif

/* OEM code pages used by ACE to store member filenames and comments. */
typedef enum {
    ACE_OEM_NONE = 0,  /* no transcoding: pass bytes through unchanged  */
    ACE_OEM_850  = 850,/* DOS Western Europe (ACE.EXE default)          */
    ACE_OEM_437  = 437 /* DOS US / Latin-1                              */
} ace_oem_t;

/*
 * Transcode *n* raw OEM bytes from code page *cp* into a UTF-8 string in
 * *out* (NUL terminated, truncated to fit *out_sz*).  Bytes < 0x80 are copied
 * verbatim; high bytes are expanded through the code page table.  *cp* equal
 * to ACE_OEM_NONE copies the bytes through (still NUL terminating).
 * Returns ACE_OK on success, ACE_ERR_PARAM on bad arguments.
 */
int ace_oem_to_utf8(const uint8_t *in, size_t n, ace_oem_t cp, char *out, size_t out_sz);

/* Parse a code page name ("850", "437", "cp850", "none"); ACE_OEM_NONE if unknown. */
ace_oem_t ace_oem_parse(const char *name);

#ifdef __cplusplus
}
#endif

#endif
