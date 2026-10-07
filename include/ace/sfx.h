/*
 * sfx.h - Self-Extracting Archive (SFX) stub definitions and conversion.
 */
#ifndef ACE_SFX_H
#define ACE_SFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ACE_SFX_NONE = 0,
    ACE_SFX_DOS,        /* Authentic 32-bit DOS PMODE/W stub (default on non-Windows) */
    ACE_SFX_WIN32_CL,   /* Authentic 32-bit Win32 Console PE stub */
    ACE_SFX_WIN32_GUI,  /* Authentic 32-bit Win32 GUI PE stub */
    ACE_SFX_LINUX       /* Native 64-bit Linux ELF standalone extractor stub */
} ace_sfx_type_t;

#ifdef _WIN32
  #define ACE_SFX_DEFAULT_TYPE ACE_SFX_WIN32_GUI
#else
  #define ACE_SFX_DEFAULT_TYPE ACE_SFX_DOS
#endif

/**
 * Returns the embedded SFX stub binary buffer and its byte size.
 * Returns NULL if type is ACE_SFX_NONE or unknown.
 */
const uint8_t *ace_sfx_get_stub(ace_sfx_type_t type, size_t *size);

/**
 * Parse a stub type name string:
 *   "dos", "dos32", "pmode" -> ACE_SFX_DOS
 *   "win", "win32", "win32cl", "cl" -> ACE_SFX_WIN32_CL
 *   "gui", "win32gui", "wingui" -> ACE_SFX_WIN32_GUI
 *   "linux", "lnx", "elf", "elf64" -> ACE_SFX_LINUX
 * Returns ACE_SFX_NONE on unrecognised name.
 */
ace_sfx_type_t ace_sfx_parse_type(const char *name);

/**
 * Return human-readable name of SFX stub type.
 */
const char *ace_sfx_type_name(ace_sfx_type_t type);

/**
 * Convert an existing ACE archive file to SFX.
 * Reads in_path, writes out_path with prepended stub.
 * If out_path is NULL, replaces .ace with .exe/.sfx or appends extension.
 */
int ace_sfx_convert_archive(const char *in_path, const char *out_path, ace_sfx_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* ACE_SFX_H */
