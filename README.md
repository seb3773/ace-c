# ACE — native C compressor/decompressor for the ACE 1.0 / 2.x archive format

A from-scratch, dependency-free reimplementation of the proprietary **ACE**
archive format (WinACE) in strict C11 for Linux, reverse-engineered directly
from the authentic closed-source legacy binaries (`ACE.EXE` PMODE/W 32-bit DOS,
`ACE32.EXE` Win32, and `UNACE.EXE`). The engine achieves **bit-for-bit exact
format conformance, identical canonical Huffman tree generation, and full
bidirectional interoperability** with genuine WinACE archives.

---

## Reverse-engineering approach and notable findings

The project followed a reverse-engineering methodology directly from authentic
legacy binaries: build the decoder until it faithfully unpacks authentic archives,
then invert each stage into an encoder and immediately validate bidirectionally
against the original DOS and Win32 binaries. Along the way, several ACE format
details turned out to be counter-intuitive and are worth documenting (for an
exhaustive technical specification, bitstream structures, and internal algorithms
reverse-engineered from Marcel Lemke's binaries, see [FORMAT.md](FORMAT.md)):

### Technical discoveries and format details

- **CRC-32 without the final XOR.** ACE uses the reflected `0xEDB88320`
  polynomial with `0xFFFFFFFF` init but **does not complement the result**, so
  `zlib.crc32()` is *not* directly usable — our `src/crc.c` implements the raw
  variant. The header checksum (CRC-16) is simply the **low 16 bits of that
  same CRC-32** over the header body, computed over the length-prefixed header
  block `<crc16, size, body...>`.
- **Comments are not a header type.** Despite early assumptions, ACE has no
  standalone `COMMENT` header record: the main comment is an inline field of
  the MAIN header (gated by `FLAG_COMMENT`), and member comments are inline
  fields right after the filename. Comments are Huffman-compressed
  independently of the member's compression method.
- **Directories are not a header type either.** A directory entry is an
  ordinary FILE32 record with the DOS `DIRECTORY` attribute set and
  `packsize = origsize = crc = 0` — including empty directories.
- **64-bit sizes (FILE64) and > 4 GiB support.** In legacy DOS/WinACE binaries
  (under PMODE/W and 32-bit Windows on FAT16/FAT32), single files $\ge 4$ GiB
  were not supported due to 32-bit DOS pointer limits and filesystem bounds.
  However, Marcel Lemke specified `ACE_TYPE_FILE64` (type 3) and `ACE_FLAG_64BIT`
  (`0x0004`) in the ACE 2.0 format specification for forward-compatibility.
  Our native 64-bit implementation supports both decoding and encoding: when
  uncompressed or compressed file sizes exceed $2^{32}-1$ bytes (or under
  `ACE_FORCE_FILE64`), `ace a` automatically promotes member records to `FILE64`
  with 64-bit size fields, verified and unpackable by `ace x`.
- **Multi-volume size semantics.** In a split archive each volume stores a
  *local* `packsize`, while `origsize` and the CRC remain **global**; the
  first volume sets `CONTNEXT`, the last sets `CONTPREV`, and a single-volume
  "split" sets both. Volume naming renames `x.ace` → `x.c00`, `x.c01`, …
- **ACE-flavoured Blowfish.** Encrypted members use a Blowfish variant with
  the π-digit P/S tables but ACE's own key schedule (encrypted with CBC and an
  8-byte pad); decryption walks the subkeys in reverse order.
- **Two Huffman families.** ACE 1.0 uses per-member static Huffman trees,
  while ACE 2.0 "blocked" streams carry tables with delta-encoded bit lengths;
  getting the table serialization right was the single hardest inversion step.
- **Blocked-mode preprocessing filters.** EXE (MZ detection + E8/E9 call
  relocation), DELTA (2- and 4-byte), SOUND (8/16/32A/32B channel deltas) and
  PIC (planar separation with configurable width/planes). The encoder's auto
  mode selection heuristics detect executable, audio, and planar bitmap streams;
  `-xe`, `-dl`, `-s8…-s32b` and `-p W[:P]` force a specific filter.
- **Solid dictionary synchronization across filtered blocks.** During decompression
  of blocked archives, the LZ77 decoder registers the *preprocessed* byte stream
  into its sliding window dictionary for DELTA (planar delta bytes) and EXE
  (jump-translated relative target bytes), whereas SOUND and PIC modes register
  the uncompressed output samples. In solid archives, the encoder must mirror
  this exact transformed state in its history buffer; referencing raw input bytes
  for dictionary matches corrupts subsequent members.
- **Degenerate Huffman trees (0 or 1 used symbols).** When an audio residual or
  sparse block contains only one active symbol, WinACE's canonical tree builder
  assigns width 1 to that single symbol in the low half of the code table
  without synthesizing a second dummy symbol.
- **64 KiB chunking constraint for DELTA blocks.** Authentic DOS WinACE enforces
  a $\le 65536$-byte segment boundary on DELTA runs. Large files must be chunked
  into contiguous $\le 65536$-byte segments, each an exact multiple of
  `delta_dist`.
- **Paths and OEM names.** Member names are stored with ACE's `\` separators
  in the creator's OEM code page; our decoder normalizes `\` → `/`, and
  cp850/cp437 name decoding is verified across character sets.

---

## Security & Standalone SFX Architecture

### Path Traversal & Zip-Slip Protection
Both `ace` and the standalone native Linux SFX stub strictly validate all extraction paths through `ace_is_safe_relpath()`:
- **Depth tracking:** Continuously monitors directory nesting depth; any attempt to ascend above the destination root via `../` (or `..\`) is intercepted and safely skipped with a security warning (`Security warning: skipping unsafe path traversal`).
- **Absolute & drive path rejection:** Rejects leading absolute slashes (`/`, `\`) and DOS drive identifiers (`C:`).
- **DOS separator normalization:** Automatically normalizes historical DOS backslashes (`DIR\SUBDIR\FILE.TXT`) into forward slashes `/` on POSIX systems, ensuring safe, seamless directory hierarchy recreation.

### Autonomous Multi-Platform SFX Stubs
The unified binary embeds authentic 32-bit DOS PMODE/W, Win32 Console, and Win32 GUI stubs, alongside a native 64-bit Linux ELF standalone extractor stub directly within the executable:
- **Direct SFX creation:** `ace a -sfx[=TYPE]` creates standalone self-extracting executables.
- **Archive conversion:** `ace s [opts] ARCHIVE [OUT]` converts existing archives to standalone SFX executables.
- **Supported stub targets:** `dos` (32-bit DOS PMODE/W), `win32` (Win32 Console PE), `gui` (Win32 GUI PE), and `linux` (native 64-bit Linux ELF).
- **Zero dependencies:** Self-extracting archives run autonomously under DOSBox, Windows, Wine, and modern Linux distributions without requiring external archive tools.

### Format Limits & Compatibility Handling
- **DOS timestamp clamping:** The DOS timestamp field carries a 7-bit year offset (1980–2107, 2-second resolution, local time, no timezone). Rather than emitting silently wrapped bits for out-of-range filesystem timestamps, the encoder clamps to the nearest representable year. The decoder gracefully tolerates out-of-bounds datetime fields from corrupted legacy archives by falling back to safe defaults rather than failing extraction.
- **Windows NT Security ACLs:** The Windows-specific NT ACL header record is recognized, surfaced in header dumps, and safely skipped during extraction on POSIX systems.

---

## Build

The project is written in strict C11 with **zero external dependencies** beyond standard libc / POSIX / Win32. Binaries are organized into dedicated target directories:

- **Linux x86_64:** `build/linux/ace` (with a convenience `./ace` symlink in the repository root).
- **Windows x86_64:** `build/win64/ace.exe` (statically linked, standalone PE executable).

### Prerequisites

| Target | Toolchain / Dependency | Debian / Ubuntu | Fedora | Arch Linux |
|---|---|---|---|---|
| **Linux (native)** | GCC $\ge$ 4.9 or Clang, GNU Make | `build-essential` | `gcc make` | `base-devel` |
| **Windows 64-bit** | MinGW-w64 (`x86_64-w64-mingw32-gcc`) | `mingw-w64` | `mingw64-gcc` | `mingw-w64-gcc` |
| **Test Suites** | Python 3 (standard library only) | `python3` | `python3` | `python` |
| **Wine Smoke Test** *(optional)* | Wine | `wine` | `wine` | `wine` |

### Compilation Commands

```sh
# 1. Build all available targets (Linux + Win64 if MinGW is detected)
make

# 2. Build for Linux only
make linux          # produces build/linux/ace and links ./ace

# 3. Cross-compile for Windows 64-bit
make win64          # produces build/win64/ace.exe

# 4. Run test suites
make test           # full 37-test Linux regression suite
make test-win64     # automated functional test of ace.exe running under Wine
python3 tests/test_real_world.py  # comprehensive corpus benchmark

# 5. Clean all build outputs
make clean          # removes build/, root symlinks, and temporary test artifacts
```

### Windows 64-bit Binary Details

The Windows executable (`build/win64/ace.exe`) is compiled with `-static`:
- **Fully autonomous:** Statically links GCC runtime helpers, requiring **zero extra DLLs** (no `libgcc_s_seh-1.dll`, no `libwinpthread-1.dll`).
- **System imports only:** Imports strictly `KERNEL32.DLL` and `MSVCRT.DLL` (standard Windows CRT).
- **Portable:** Can be dropped as a single `.exe` onto any fresh 64-bit Windows installation (Windows 7 through 11, Windows Server) and runs out of the box.
- **Native attribute support:** Uses native Win32 APIs (`GetFileAttributesA` / `SetFileAttributesA`) to preserve and restore all historical DOS/Windows attributes (`READONLY`, `HIDDEN`, `SYSTEM`, `ARCHIVE`).
- **Contextual default SFX:** On Windows, `-sfx` automatically defaults to the authentic Win32 GUI PE stub (`WIN32GUI`), creating double-clickable extractors.

> **Portability note:** Strict C11 mode with GCC 14 treats implicit declarations as
> errors; POSIX APIs are enabled via explicit feature-test macros (`_DEFAULT_SOURCE`)
> at the top of affected source files rather than via `-std=gnu11`.

---

## Usage

The project builds a single unified **`ace`** binary (mirroring the classic DOS `ACE.EXE` tool, without the `.exe` extension):

```text
ace a ARCHIVE [opts] FILE...   create archive
ace s [opts] ARCHIVE [OUT]     convert archive to SFX (.exe / .sfx)
ace x [opts] ARCHIVE [PAT...]  extract files with full pathnames
ace e [opts] ARCHIVE [PAT...]  extract files without pathnames (junk paths)
ace l [opts] ARCHIVE [PAT...]  list archive contents
ace v [opts] ARCHIVE [PAT...]  verbose list archive contents
ace t [opts] ARCHIVE [PAT...]  test archive integrity
ace d [opts] ARCHIVE           dump raw archive headers
ace h                          help
```

### Create

```sh
./ace a MY.ace -z     file.txt            # LZ77 (ACE 1.0 method)
./ace a MY.ace -2     folder/             # blocked ACE 2.0, auto filter, recursive
./ace a MY.ace -s -2  a.bin b.bin         # solid archive (shared dictionary)
./ace a MY.ace -2 -xe prog.exe            # force EXE preprocessing
./ace a MY.ace -2 -dl sound.raw           # force DELTA
./ace a MY.ace -s16   audio.wav           # blocked SOUND_16
./ace a MY.ace -2 -p 640:3 image.rgb      # blocked PIC (width:planes)
./ace a MY.ace -z -pw SECRET -cm "note" notes.txt  # Blowfish + main comment
./ace a BIG.ace -z -V 131072 big.iso      # multi-volume (.c00 .c01 … ≤ 128 KiB)
./ace a MY.ace -A -z ./some/deep/path     # keep full path (ACE backslash style)
./ace a MY.exe -sfx   file.txt            # create authentic DOS 32-bit PMODE/W SFX
./ace a MY.exe -sfx=win32 file.txt        # create authentic Win32 Console PE SFX
./ace a MY.exe -sfx=gui file.txt          # create authentic Win32 GUI PE SFX
./ace a MY.sfx -sfx=linux file.txt        # create native 64-bit Linux ELF standalone SFX
```

Creation options: `-0` store · `-z` LZ77 · `-2` blocked · `-s` solid ·
`-s8/-s16/-s32a/-s32b` sound · `-xe`/`-dl` force EXE/DELTA · `-p W[:P]` PIC ·
`-m 0-5` quality · `-d KB` dictionary (32…4096) · `-pw` password ·
`-cm`/`-cf` main/member comments · `-V` volume size · `-A` full paths ·
`-k` lock archive · `-x PAT` exclude files (`-x@LIST` for file) · `@LIST` file list ·
`-sfx[=TYPE]` self-extracting archive (types: `dos`, `win32`, `gui`, `linux`) ·
directories archived recursively (DOS attributes + mtime preserved).

### Convert to SFX

```sh
./ace s MY.ace                            # converts MY.ace -> MY.exe (default DOS stub)
./ace s -sfx=win32 MY.ace                 # converts to Win32 Console SFX
./ace s -sfx=gui MY.ace CUSTOM.exe        # converts to Win32 GUI SFX with custom output name
./ace s -sfx=linux MY.ace                 # converts MY.ace -> MY.sfx (native Linux ELF stub)
```

### Extract & List

```sh
./ace l MY.ace                  # tabular file listing
./ace l -1 MY.ace               # bare listing (one path per line: scripts, xargs, grep)
./ace l -u MY.ace               # format paths as Unix relative paths (/ slashes, lowercase)
./ace l -1 -u MY.ace            # bare list of lowercase Unix paths
./ace l MY.ace '*.txt'          # case-insensitive wildcard pattern selection
./ace x MY.ace                  # extract, keeping the stored tree
./ace x -u MY.ace               # extract converting uppercase DOS names to lowercase
./ace x -j MY.ace               # "junk" paths (flatten into target directory)
./ace x -k MY.ace               # restore mtime + attributes
./ace x -d OUT MY.ace           # destination directory
./ace x -p SECRET MY.ace        # Blowfish decryption
./ace x --oem 850 MY.ace        # OEM name decoding (cp850 / cp437)
```

Extraction & listing options:
- `-1`, `--bare`: Bare output format (one path per line, script/pipe friendly).
- `-u`, `--unix`: Format paths as Unix relative paths (`/` slashes, lowercase).
- `-j`: Strip stored directory names on extract (flat extraction).
- `-k`: Restore original file timestamps and attributes.
- `-d DIR`: Destination extraction directory.
- `-p PASS`: Decryption password.
- `--oem CP`: Decode OEM member names from code pages 850 or 437.
- `-v`: Verbose listing / extraction output.

---

## Scope

### Supported
- **Header types**: MAIN (0), FILE32 (1), FILE64 (3), recovery (2/4/5,
  recognized). Comments inline; directories as FILE32 + `DIRECTORY` attribute.
- **Compression**: STORE · LZ77 (ACE 1.0) · **blocked ACE 2.0** with **EXE**,
  **DELTA**, **SOUND** (8/16/32A/32B) and **PIC** preprocessing, auto-selected
  or forced.
- **Huffman**: static (ACE 1.0) and adaptive/blocked 2.0 tables.
- **Encryption**: ACE-variant Blowfish, CBC, 8-byte padding (`-pw`).
- **Multi-volume** encode and decode (`CONTNEXT`/`CONTPREV`, `.c00…`).
- **Solid** archives (dictionary shared across members).
- **Comments**: main *and* per-member, orthogonal to compression method.
- **Directories**: entries (including empty) + recursive traversal on create.
- **DOS metadata**: attributes (`READONLY`/`ARCHIVE`/`DIRECTORY`…) and
  `datetime` mapped from/to `st_mtime`.
- **OEM names** (long file names included) in cp850 / cp437.
- **SFX (Self-Extracting Archives)**: Direct authoring via `ace a -sfx[=TYPE]`
  and standalone conversion via `ace s [opts] ARCHIVE [OUT.EXE/OUT.sfx]`.
  Includes authentic embedded stubs: 32-bit DOS PMODE/W (`DOS32`),
  Win32 Console (`WIN32CL`), Win32 GUI (`WIN32GUI`), and native 64-bit Linux ELF (`LINUX`),
  fully autonomous under DOSBox, Windows, Wine, and modern Linux distributions.

### Out of scope / Format limits
- **NT Security ACLs**: Windows/NTFS-specific records are recognized in headers and safely skipped on Linux.
- **DOS timestamp limits**: 1980–2107 range clamped safely without bit wrap; resilient decoding of corrupted legacy dates.

---

## Cross-validation DOSBox ↔ authentic WinACE 2.6

Bidirectional empirical validation against the genuine `ACE.EXE` v2.6 (32-bit DOS)
executed under DOSBox.

### Our compressor → read by the real ACE
All 8 archives produced by our Linux `ace a` were listed (`L`) **and integrity-tested
(`T`) with `CRC OK`** by WinACE 2.6:

| Archive (built on Linux) | Content | ACE 2.6 `T -y` verdict |
|---|---|---|
| `STORE.ACE`   | HELLO.TXT (store)           | `CRC OK` |
| `LZ77.ACE`    | LZ77 ACE 1.0, 2 members       | `CRC OK` ×2 |
| `BLOCKED.ACE` | blocked auto, 3 members       | `CRC OK` ×3 |
| `SOLID.ACE`   | shared dictionary             | `CRC OK` ×2 |
| `DELTA.ACE`   | forced DELTA filter           | `CRC OK` |
| `EXE.ACE`     | forced EXE filter             | `CRC OK` |
| `PIC.ACE`     | PIC mode (`-p 3`)             | `CRC OK` |
| `CM.ACE`      | main+member comments          | `CRC OK` — and ACE **prints our Linux-authored comment**: `Main comment: MAIN COMMENT FROM LINUX ace` |

### Real ACE → read by our decoder
`ACE.EXE A` produced `DOSMADE.ACE`; our `ace t` tests it (`CHECK.TXT: OK`),
and `ace x` extracts it (`CHECK.TXT` matching bit-for-bit).

### Reproduce
```sh
sh tools/gen_dosbox_verify.sh     # regenerates tools/dosbox-verify/ (archives + VERIFY.BAT)
#   then, inside DOSBox (interactive):
#     mount c <…>/tools/dosbox-verify
#     c:
#     VERIFY.BAT                  # -> RESULTS.TXT (+ DOSMADE.ACE)
sh tools/check_dosbox_verify.sh   # forward (our archives) + reverse (DOSMADE.ACE)
```

### Real-World Corpus Validation Benchmark (`tests/test_real_world.py`)

A comprehensive automated benchmark validates bidirectional compression, decompression,
integrity, and cross-compatibility between our native Linux implementation (`ace`)
and the authentic Marcel Lemke WinACE 2.6 (`ACE.EXE` PMODE/W under DOSBox)
across 7 diverse real-world files:

| Member | Format / Content | Size | Packed | Ratio | Active Filter | Verified |
|---|---|---:|---:|---:|---|:---:|
| `APP.EXE` | 16/32-bit DOS Executable | 62,012 B | 61,256 B | 98.9% | x86 EXE | Linux + DOSBox OK |
| `AUDIO.WAV` | 16-bit 44.1kHz Stereo PCM Audio | 200,044 B | 111,900 B | 56.0% | SOUND_32A | Linux + DOSBox OK |
| `CODE.PY` | Python Source Code | 10,460 B | 3,148 B | 30.1% | Plain LZ77 | Linux + DOSBox OK |
| `DATA.DAT` | Periodic Telemetry Database | 100,000 B | 348 B | 0.4% | DELTA ($D=16$) | Linux + DOSBox OK |
| `DOC.TXT` | English Technical Manual | 64,513 B | 18,232 B | 28.4% | Plain LZ77 | Linux + DOSBox OK |
| `IMAGE.BMP` | 24-bit RGB Bitmap Raster (320x240) | 230,454 B | 157,112 B | 68.2% | PIC 2D | Linux + DOSBox OK |
| `WIN.EXE` | Win32 Portable Executable (PE) | 45,568 B | 43,916 B | 96.6% | x86 EXE | Linux + DOSBox OK |

1. **Individual Compression**: All 7 files compressed individually with `ace a -2 -m5` and verified by both our decoder and authentic DOSBox `ACE.EXE t -y TEST.ACE`.
2. **Multi-File Solid Archive (`REALCORP.ACE`)**:
   - Compresses all 7 files (713,051 bytes) into a single solid archive of **389,848 bytes (54.7%)** in 0.29s.
   - Verified with CRC OK across all 7 members in both Linux `ace t` and authentic DOSBox `ACE.EXE`.
   - Extracted with `ace x` in 0.022s with **100% bit-exact SHA-256 match** on all 7 files.
3. **Reverse Cross-Validation (Authentic ACE.EXE → Our Decoder)**:
   - Genuine `ACE.EXE` running in DOSBox creates solid archives (`DOSGRP1.ACE` and `DOSGRP2.ACE`).
   - Our native Linux `ace t` validates CRC on all members.
   - Our native Linux `ace x` extracts all members with **100% bit-exact SHA-256 match** against original files.

Run the real-world benchmark anytime with:
```sh
python3 tests/test_real_world.py
```

## Regression suite (`make test`)

```text
test_core                    units: CRC, bitstream, Huffman, LZ77, Blowfish
roundtrip codecs             blocked · sound8/16/32a · pic · pic_left · lz77 · solid · enc
mv.ace                       multi-volume fixture verification
cli_extras                   wildcards, -j (junk paths), -k (mtime/attributes restore)
oem                          cp850/cp437 member names (transcoding fidelity)
multi-volume encode          multi-volume volume splitting and follow-on reassembly
forced filter modes          forced EXE (-xe) and DELTA (-dl) encoding modes
metadata preservation        DOS attributes and st_mtime roundtrip fidelity
path preservation            full paths / LFN storage (-A) and reconstruction
comments                     main (-cm) and member (-cf) inline comments
recursive directory trees    tree archiving with empty directory entry preservation
datetime bounds              encoder clamp 1980–2107 + resilient decoding of corrupt fields
ace unified CLI              full command suite (a, x, e, l, v, t, d)
archive locking              archive locking flag preservation (-k)
FILE64 support               large 64-bit member encoding (> 4 GiB format)
exclusions & listfiles       pattern exclusions (-x) and @listfile input
sfx_stubs                    authentic DOS/Win32 & native Linux SFX creation + conversion
path_traversal_security      rejection of ../, drive identifiers & backslash normalization
bare_and_unix_listing        -1 bare and -u unix path formatting (CLI + Linux SFX)
winace_corpus                9 authentic WinACE archives unpacked + bit-exact recompression
```

`make test` runs 37 automated checks with 0 failures and leaves no
artefacts behind.

---

## Repository layout

```text
include/ace/    archive · bitstream · blowfish · compress · crc · engine ·
                huffman · lz77 · oem · pic · sound · util · cli
src/            engine modules + ace.c (unified front-end) · mkace.c · unace.c
tests/          test_core.c · test_roundtrip.py · test_real_world.py ·
                winace_corpus.py · fixture generators (.py)
tools/          gen_dosbox_verify.sh · check_dosbox_verify.sh · gen_sfx_stubs.py ·
                dosbox-ace.sh · wine-ace.sh
research/       original DOS & Win32 binaries
testdata/       fixtures · winace/ (authentic corpus) · dosbox-verify/ (suite)
```

## License

See [LICENSE](LICENSE). This is an educational reverse-engineering project;
ACE is a proprietary format and trademark. The bundled WinACE binaries and
archives are used solely for interoperability testing and belong to their
authors.
