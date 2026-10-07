#!/usr/bin/env python3
"""
Generate C source file src/sfx_stubs.c and header include/ace/sfx.h
embedding authentic DOS 32-bit, Win32 Console, and Win32 GUI SFX stubs.
"""

import os
import sys

import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DOS_STUB_PATH = os.path.join(ROOT, "research", "binDos", "UNACE.EXE")
WIN_STUB_PATH = os.path.join(ROOT, "research", "binDos", "UNACE32.EXE")
GUI_STUB_PATH = os.path.join(ROOT, "research", "binDos", "SFXFILES", "WIN32GUI.SFX")

def build_linux_stub():
    linux_src = os.path.join(ROOT, "src", "sfx_linux_stub.c")
    out_bin = os.path.join(ROOT, "build", "sfx_linux_stub")
    os.makedirs(os.path.join(ROOT, "build"), exist_ok=True)
    c_files = [
        linux_src,
        os.path.join(ROOT, "src", "archive.c"),
        os.path.join(ROOT, "src", "compress.c"),
        os.path.join(ROOT, "src", "engine.c"),
        os.path.join(ROOT, "src", "lz77.c"),
        os.path.join(ROOT, "src", "huffman.c"),
        os.path.join(ROOT, "src", "sound.c"),
        os.path.join(ROOT, "src", "pic.c"),
        os.path.join(ROOT, "src", "blowfish.c"),
        os.path.join(ROOT, "src", "blowfish_tables.c"),
        os.path.join(ROOT, "src", "bitstream.c"),
        os.path.join(ROOT, "src", "crc.c"),
        os.path.join(ROOT, "src", "oem.c"),
        os.path.join(ROOT, "src", "util.c"),
    ]
    cmd = [
        "gcc", "-std=c11", "-Wall", "-Wextra", "-Wno-unused-parameter",
        "-Os", "-s", "-ffunction-sections", "-fdata-sections",
        "-Wl,--gc-sections", "-Wl,--build-id=none",
        f"-I{os.path.join(ROOT, 'include')}",
        *c_files,
        "-o", out_bin
    ]
    print("Compiling standalone Linux SFX stub...")
    subprocess.run(cmd, check=True)
    with open(out_bin, "rb") as f:
        data = f.read()
    assert data.startswith(b"\x7fELF"), "Linux stub must be an ELF executable"
    return data

def read_stubs():
    with open(DOS_STUB_PATH, "rb") as f:
        dos_bytes = f.read()

    with open(WIN_STUB_PATH, "rb") as f:
        win_bytes = f.read()

    with open(GUI_STUB_PATH, "rb") as f:
        f.seek(2590)  # MZ offset in WIN32GUI.SFX
        gui_bytes = f.read()

    linux_bytes = build_linux_stub()

    assert dos_bytes.startswith(b"MZ"), "DOS stub must start with MZ"
    assert win_bytes.startswith(b"MZ"), "Win32 stub must start with MZ"
    assert gui_bytes.startswith(b"MZ"), "Win32 GUI stub must start with MZ"
    assert linux_bytes.startswith(b"\x7fELF"), "Linux stub must start with \\x7fELF"

    return dos_bytes, win_bytes, gui_bytes, linux_bytes

def format_c_array(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    chunk_size = 16
    for i in range(0, len(data), chunk_size):
        chunk = data[i:i + chunk_size]
        hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + chunk_size < len(data):
            lines.append(f"    {hex_vals},")
        else:
            lines.append(f"    {hex_vals}")
    lines.append("};")
    return "\n".join(lines)

def generate_header():
    header_path = os.path.join(ROOT, "include", "ace", "sfx.h")
    content = """/*
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
    ACE_SFX_DOS,        /* Authentic 32-bit DOS PMODE/W stub (default) */
    ACE_SFX_WIN32_CL,   /* Authentic 32-bit Win32 Console PE stub */
    ACE_SFX_WIN32_GUI,  /* Authentic 32-bit Win32 GUI PE stub */
    ACE_SFX_LINUX       /* Native 64-bit Linux ELF standalone extractor stub */
} ace_sfx_type_t;

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
"""
    with open(header_path, "w") as f:
        f.write(content)
    print("Wrote", header_path)

def generate_source(dos_bytes, win_bytes, gui_bytes, linux_bytes):
    src_path = os.path.join(ROOT, "src", "sfx_stubs.c")
    print("Formatting C arrays (approx 211 KB)...")
    dos_arr = format_c_array("sfx_dos_stub", dos_bytes)
    win_arr = format_c_array("sfx_win32_cl_stub", win_bytes)
    gui_arr = format_c_array("sfx_win32_gui_stub", gui_bytes)
    linux_arr = format_c_array("sfx_linux_stub", linux_bytes)

    c_content = f"""/*
 * sfx_stubs.c - Authentic embedded SFX stubs (DOS 32-bit, Win32 CL, Win32 GUI, Linux 64-bit).
 * Auto-generated by tools/gen_sfx_stubs.py.
 */
#include "ace/sfx.h"
#include "ace/archive.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

{dos_arr}

{win_arr}

{gui_arr}

{linux_arr}

const uint8_t *ace_sfx_get_stub(ace_sfx_type_t type, size_t *size)
{{
    switch (type) {{
    case ACE_SFX_DOS:
        if (size) *size = sizeof(sfx_dos_stub);
        return sfx_dos_stub;
    case ACE_SFX_WIN32_CL:
        if (size) *size = sizeof(sfx_win32_cl_stub);
        return sfx_win32_cl_stub;
    case ACE_SFX_WIN32_GUI:
        if (size) *size = sizeof(sfx_win32_gui_stub);
        return sfx_win32_gui_stub;
    case ACE_SFX_LINUX:
        if (size) *size = sizeof(sfx_linux_stub);
        return sfx_linux_stub;
    default:
        if (size) *size = 0;
        return NULL;
    }}
}}

ace_sfx_type_t ace_sfx_parse_type(const char *name)
{{
    if (!name || !*name)
        return ACE_SFX_DOS;
    if (strcasecmp(name, "dos") == 0 || strcasecmp(name, "dos32") == 0 ||
        strcasecmp(name, "pmode") == 0)
        return ACE_SFX_DOS;
    if (strcasecmp(name, "win") == 0 || strcasecmp(name, "win32") == 0 ||
        strcasecmp(name, "win32cl") == 0 || strcasecmp(name, "cl") == 0)
        return ACE_SFX_WIN32_CL;
    if (strcasecmp(name, "gui") == 0 || strcasecmp(name, "win32gui") == 0 ||
        strcasecmp(name, "wingui") == 0)
        return ACE_SFX_WIN32_GUI;
    if (strcasecmp(name, "linux") == 0 || strcasecmp(name, "lnx") == 0 ||
        strcasecmp(name, "elf") == 0 || strcasecmp(name, "elf64") == 0)
        return ACE_SFX_LINUX;
    return ACE_SFX_NONE;
}}

const char *ace_sfx_type_name(ace_sfx_type_t type)
{{
    switch (type) {{
    case ACE_SFX_DOS:       return "DOS-32bit (PMODE/W)";
    case ACE_SFX_WIN32_CL:  return "Win32 (Commandline)";
    case ACE_SFX_WIN32_GUI: return "Win32 (GUI)";
    case ACE_SFX_LINUX:     return "Linux-x86_64 (ELF)";
    default:                return "none";
    }}
}}

int ace_sfx_convert_archive(const char *in_path, const char *out_path, ace_sfx_type_t type)
{{
    char default_out[4096];
    const char *target;
    size_t stub_sz = 0;
    const uint8_t *stub;
    FILE *fin, *fout;
    uint8_t buf[65536];
    size_t n;
    int is_same = 0;

    if (!in_path)
        return ACE_ERR_PARAM;
    if (type == ACE_SFX_NONE)
        type = ACE_SFX_DOS;

    stub = ace_sfx_get_stub(type, &stub_sz);
    if (!stub || stub_sz == 0)
        return ACE_ERR_PARAM;

    fin = fopen(in_path, "rb");
    if (!fin) {{
        perror(in_path);
        return ACE_ERR_IO;
    }}

    /* Check if already SFX (MZ or ELF) */
    if (fread(buf, 1, 4, fin) >= 2) {{
        if ((buf[0] == 'M' && buf[1] == 'Z') ||
            (buf[0] == 0x7F && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F')) {{
            fclose(fin);
            fprintf(stderr, "%s is already a self-extracting archive\\n", in_path);
            return ACE_ERR_PARAM;
        }}
    }}
    rewind(fin);

    if (!out_path) {{
        size_t len = strlen(in_path);
        if (len >= sizeof(default_out))
            len = sizeof(default_out) - 5;
        memcpy(default_out, in_path, len);
        default_out[len] = '\\0';
        const char *ext = (type == ACE_SFX_LINUX) ? ".sfx" : ".exe";
        if (len > 4 && strcasecmp(default_out + len - 4, ".ace") == 0) {{
            memcpy(default_out + len - 4, ext, 4);
        }} else {{
            strncat(default_out, ext, sizeof(default_out) - len - 1);
        }}
        target = default_out;
    }} else {{
        target = out_path;
    }}

    if (strcmp(in_path, target) == 0) {{
        is_same = 1;
        snprintf(default_out, sizeof(default_out), "%s.tmp_sfx", in_path);
        target = default_out;
    }}

    fout = fopen(target, "wb");
    if (!fout) {{
        perror(target);
        fclose(fin);
        return ACE_ERR_IO;
    }}

    /* 1. Write SFX stub */
    if (fwrite(stub, 1, stub_sz, fout) != stub_sz) {{
        fclose(fin);
        fclose(fout);
        unlink(target);
        return ACE_ERR_IO;
    }}

    /* 2. Copy ACE archive body */
    while ((n = fread(buf, 1, sizeof(buf), fin)) > 0) {{
        if (fwrite(buf, 1, n, fout) != n) {{
            fclose(fin);
            fclose(fout);
            unlink(target);
            return ACE_ERR_IO;
        }}
    }}

    fclose(fin);
    fclose(fout);

    if (is_same) {{
        if (rename(target, in_path) != 0) {{
            perror("rename");
            return ACE_ERR_IO;
        }}
        target = in_path;
    }}

    /* Grant executable permission */
    chmod(target, 0755);
    return ACE_OK;
}}
"""
    with open(src_path, "w") as f:
        f.write(c_content)
    print("Wrote", src_path, f"({len(c_content)} bytes)")

def main():
    dos_bytes, win_bytes, gui_bytes, linux_bytes = read_stubs()
    print(f"Loaded stubs: DOS={len(dos_bytes)}B, WIN={len(win_bytes)}B, GUI={len(gui_bytes)}B, LINUX={len(linux_bytes)}B")
    generate_header()
    generate_source(dos_bytes, win_bytes, gui_bytes, linux_bytes)
    print("SFX stubs generation complete.")

if __name__ == "__main__":
    main()
