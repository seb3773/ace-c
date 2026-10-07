# Technical Specification and Reverse Engineering of the ACE Format (WinACE 2.x & DOS ACE)

This document provides a comprehensive technical architecture, binary structure specification, and internal compression algorithms of the **ACE** archiver (v1.x and v2.x), derived from reverse engineering Marcel Lemke's official binaries (`ACE.EXE` PMODE/W 32-bit DOS and `ACE32.EXE` Win32).

---

## Table of Contents

1. [Archive Structure Overview](#1-archive-structure-overview)
   - [Standard ACE Stream](#standard-ace-stream)
   - [Self-Extracting Archives (SFX) and Executable Stubs](#self-extracting-archives-sfx-and-executable-stubs)
2. [Binary Headers](#2-binary-headers)
   - [Main Header (MAIN HEADER)](#main-header-main-header)
   - [File Header (FILE32 HEADER)](#file-header-file32-header)
   - [64-bit File Header (FILE64 HEADER) and > 4 GiB Format Extension](#64-bit-file-header-file64-header-and--4-gib-format-extension)
   - [Header CRC-16 Calculation](#header-crc-16-calculation)
   - [MS-DOS 32-bit Timestamp](#ms-dos-32-bit-timestamp)
3. [Bitstream Architecture](#3-bitstream-architecture)
   - [Bit Ordering and 32-bit Accumulator](#bit-ordering-and-32-bit-accumulator)
   - [Huffman Tree Serialization](#huffman-tree-serialization)
   - [Degenerate Huffman Trees (Single-Symbol Codes)](#degenerate-huffman-trees-single-symbol-codes)
   - [Lemke's Unstable Quicksort & Canonical Tie-Breaking](#lemkes-unstable-quicksort--canonical-tie-breaking)
4. [LZ77 Match Finder Engine](#4-lz77-match-finder-engine)
   - [The Three Hash Tables](#the-three-hash-tables)
   - [Distance History (Repeat Distances)](#distance-history-repeat-distances)
   - [Lazy Matching Evaluation](#lazy-matching-evaluation)
   - [EOF Literal Reservation Rule](#eof-literal-reservation-rule)
5. [Blocked Modes and ACE 2.0 Preprocessors](#5-blocked-modes-and-ace-20-preprocessors)
   - [Mode Switching (`TYPECODE 283`)](#mode-switching-typecode-283)
   - [DELTA Filter (Mode 1)](#delta-filter-mode-1)
   - [64 KiB Chunking Constraint for DELTA Blocks](#64-kib-chunking-constraint-for-delta-blocks)
   - [Solid Dictionary Synchronization Across Filtered Modes](#solid-dictionary-synchronization-across-filtered-modes)
   - [x86 Executable Filter (Mode 2)](#x86-executable-filter-mode-2)
   - [SOUND Audio Model (Modes 3-6)](#sound-audio-model-modes-3-6)
   - [PIC 2D Raster Filter (Mode 7)](#pic-2d-raster-filter-mode-7)
6. [Automatic Mode Selection Heuristic](#6-automatic-mode-selection-heuristic)
   - [Bit Cost Estimation](#bit-cost-estimation)
   - [The 15/16 Preference Factor](#the-1516-preference-factor)
   - [Cost-Benefit Threshold against Mode Switch Overhead](#cost-benefit-threshold-against-mode-switch-overhead)
   - [Discriminating Audio Predictors from Structured DELTA](#discriminating-audio-predictors-from-structured-delta)
7. [Cross-Validation and Empirical Verification](#7-cross-validation-and-empirical-verification)
   - [DOSBox PMODE/W WinACE 2.6 Interoperability](#dosbox-pmodew-winace-26-interoperability)
   - [Real-World Corpus Validation Benchmark](#real-world-corpus-validation-benchmark)

---

## 1. Archive Structure Overview

### Standard ACE Stream

An ACE archive is a continuous sequence of typed blocks. Every block begins with a block size and a CRC-16 checksum.

```text
+-------------------------+
|   MAIN HEADER (Type 0)  |  Archive metadata, flags, volume number, timestamps
+-------------------------+
|   FILE HEADER (Type 1)  |  Member 1 metadata (name, size, packed size, crc32, method)
+-------------------------+
|   COMPRESSED PAYLOAD 1  |  Member 1 compressed stream (LZ77, Delta, Exe...)
+-------------------------+
|   FILE HEADER (Type 1)  |  Member 2 metadata
+-------------------------+
|   COMPRESSED PAYLOAD 2  |  Member 2 compressed stream
+-------------------------+
|   ...                   |
+-------------------------+
|   RECOVERY / SECURITY   |  (Optional) Recovery records or NTFS ACL security streams
+-------------------------+
```

### Self-Extracting Archives (SFX) and Executable Stubs

A Self-Extracting ACE archive (SFX) is created by prepending an autonomous executable decompression stub directly before the standard ACE archive stream:

```text
+-------------------------+
|   SFX EXECUTABLE STUB   |  MZ/PE binary (DOS PMODE/W, Win32 CL, or Win32 GUI)
|   (62 KB / 45 KB / 61 KB)|
+-------------------------+
|   MAIN HEADER (Type 0)  |  Starts with "**ACE**" magic string
+-------------------------+
|   FILE HEADER (Type 1)  |  Member 1 metadata
+-------------------------+
|   COMPRESSED PAYLOAD 1  |  Member 1 stream
+-------------------------+
|   ...                   |
+-------------------------+
```

#### Magic Signature Forward Scanning
Standard ACE decoders (and embedded SFX stubs) do not assume the `MAIN HEADER` begins at file offset 0. Instead, decoders scan the initial file prefix (typically up to 512 KiB) searching for the 7-byte ASCII magic signature:
```text
"**ACE**"  (0x2A 0x2A 0x41 0x43 0x45 0x2A 0x2A)
```
The byte immediately preceding this signature is the 1-byte header type (`0x00` for `MAIN_HEADER`), with the preceding 2 bytes representing the header size and the first 2 bytes representing the CRC-16.

#### Zero Offset Relocation
Unlike ZIP or TAR SFX files which often embed absolute directory offsets requiring binary fixups, all internal block references in an ACE archive stream are sequential and relative: each header directly specifies `head_size` and `packsize`. Consequently, prepending an executable stub requires **zero offset adjustments** or table patches.

#### Embedded Decompression Stubs
The archiver embeds four stubs:
1. **DOS 32-bit (`DOS32.SFX`)**: A 62,012-byte PMODE/W protected mode 32-bit x86 executable (identical to `UNACE.EXE`). Features Long File Name (LFN) support and executes cleanly under MS-DOS, DOSBox, or FreeDOS without 640 KB conventional memory constraints.
2. **Win32 Console (`WIN32CL.SFX`)**: A 45,568-byte 32-bit PE console executable (identical to `UNACE32.EXE`) for batch scripting, Windows command prompt, and Wine.
3. **Win32 GUI (`WIN32GUI.SFX`)**: A 61,163-byte Petite-compressed 32-bit Win32 GUI executable providing a dialog interface for graphical archive extraction.
4. **Linux 64-bit (`sfx_linux_stub`)**: A 43-KB standalone 64-bit ELF binary (`x86_64`) that inspects `/proc/self/exe` to locate the appended ACE archive. Provides autonomous extraction, listing (`-l`), testing (`-t`), password decryption (`-p`), and path traversal security without external runtime dependencies.

---

## 2. Binary Headers

All multi-byte integers are stored in **Little-Endian** byte order.

### Main Header (MAIN HEADER)

| Offset | Size | Type | Description |
| :---: | :---: | :---: | :--- |
| `0x00` | 2 | `uint16` | **CRC-16** of the block (computed from offset 4 to the end of the header) |
| `0x02` | 2 | `uint16` | **Header size** (excludes these first 4 bytes) |
| `0x04` | 1 | `uint8` | **Block type** (`0x00` = MAIN HEADER) |
| `0x05` | 2 | `uint16` | **Header flags** (bit 12: comment present, bit 11: multi-volume...) |
| `0x07` | 7 | `bytes` | **Magic signature**: `**ACE**` (`0x2A 0x2A 0x41 0x43 0x45 0x2A 0x2A`) |
| `0x0E` | 1 | `uint8` | **Extract version required** (e.g., `0x14` = 2.0) |
| `0x0F` | 1 | `uint8` | **Creation version** (e.g., `0x14` = 2.0) |
| `0x10` | 1 | `uint8` | **Host system** (`0` = DOS, `1` = OS/2, `2` = Win32, `3` = Unix...) |
| `0x11` | 1 | `uint8` | **Volume number** (`0` for the first volume) |
| `0x12` | 4 | `uint32` | **MS-DOS Creation Timestamp** |
| `0x16` | 8 | `bytes` | **Reserved signature (`res1`)**: registration/timestamp signature |
| `0x1E` | ... | ... | Optional fields (AV verification string, archive main comment) |

### File Header (FILE32 HEADER)

| Offset | Size | Type | Description |
| :---: | :---: | :---: | :--- |
| `0x00` | 2 | `uint16` | **CRC-16** of the block |
| `0x02` | 2 | `uint16` | **Header size** |
| `0x04` | 1 | `uint8` | **Block type** (`0x01` = FILE32 HEADER) |
| `0x05` | 2 | `uint16` | **File flags** (`0x0001` = standard file, `0x0002` = encrypted...) |
| `0x07` | 4 | `uint32` | **Compressed size (Packed Size)** |
| `0x0B` | 4 | `uint32` | **Original uncompressed size** |
| `0x0F` | 4 | `uint32` | **MS-DOS Modification Timestamp (`st_mtime`)** |
| `0x13` | 4 | `uint32` | **DOS File Attributes** (Read-Only: `0x01`, Directory: `0x10`, Archive: `0x20`...) |
| `0x17` | 4 | `uint32` | **CRC-32 of uncompressed data** |
| `0x1B` | 1 | `uint8` | **Compression type**: `0` = STORED, `1` = LZ77 (ACE 1.0), `2` = BLOCKED (ACE 2.0) |
| `0x1C` | 1 | `uint8` | **Compression quality**: `0` = Fast, `1` = Normal, `2` = Good, `3` = Best |
| `0x1D` | 2 | `uint16` | **Dictionary parameters** (`fparams`: bits 0..3 define dictionary window size) |
| `0x1F` | 2 | `uint16` | **File name length** (`namelen`) |
| `0x21` | `namelen` | `bytes` | **File name** (OEM cp850 / cp437 encoding) |
| `...` | ... | ... | Optional member file comment |

### 64-bit File Header (FILE64 HEADER) and > 4 GiB Format Extension

| Offset | Size | Type | Description |
| :---: | :---: | :---: | :--- |
| `0x00` | 2 | `uint16` | **CRC-16** of the block |
| `0x02` | 2 | `uint16` | **Header size** |
| `0x04` | 1 | `uint8` | **Block type** (`0x03` = FILE64 HEADER) |
| `0x05` | 2 | `uint16` | **File flags** (includes `FLAG_64BIT` = `0x0004` and `FLAG_ADDSIZE` = `0x0001`) |
| `0x07` | 8 | `uint64` | **Compressed size (Packed Size)** (64-bit Little-Endian) |
| `0x0F` | 8 | `uint64` | **Original uncompressed size** (64-bit Little-Endian) |
| `0x17` | 4 | `uint32` | **MS-DOS Modification Timestamp (`st_mtime`)** |
| `0x1B` | 4 | `uint32` | **DOS File Attributes** |
| `0x1F` | 4 | `uint32` | **CRC-32 of uncompressed data** |
| `0x23` | 1 | `uint8` | **Compression type**: `0` = STORED, `1` = LZ77, `2` = BLOCKED |
| `0x24` | 1 | `uint8` | **Compression quality**: `0`..`5` |
| `0x25` | 2 | `uint16` | **Dictionary parameters** (`fparams`) |
| `0x27` | 2 | `uint16` | **File name length** (`namelen`) |
| `0x29` | `namelen` | `bytes` | **File name** (OEM cp850 / cp437 encoding) |
| `...` | ... | ... | Optional member file comment |

#### Historical Evolution and 64-Bit Extension Semantics
- **Legacy Limitations**: The official DOS binaries (`ACE.EXE` running under PMODE/W) and 32-bit Windows executables (`ACE32.EXE`) were built for 32-bit protected-mode environments operating primarily on FAT16, FAT32, or early NTFS filesystems. Because DOS PMODE/W relied on standard 32-bit DOS system calls (`lseek` with 32-bit signed offsets), legacy binaries were inherently incapable of seeking, addressing, or compressing individual files $\ge 4$ GiB.
- **Lemke's Specification Design**: Anticipating future filesystem growth, Marcel Lemke reserved `ACE_TYPE_FILE64` (type 3), `ACE_TYPE_RECOVERY64A` (type 4), `ACE_TYPE_RECOVERY64B` (type 5), and the flag `ACE_FLAG_64BIT` (`0x0004`) in the ACE 2.0 specification.
- **Consistency Constraint**: A conforming ACE decoder strictly enforces that `FLAG_64BIT` must appear **if and only if** the header type is `ACE_TYPE_FILE64`. If `FLAG_64BIT` appears in a `FILE32` record, or is omitted from a `FILE64` record, the archive is rejected as corrupted.
- **Native Implementation**: Our native 64-bit Linux implementation automatically promotes any file member with uncompressed or compressed size $> 2^{32} - 1$ bytes to `FILE64`.

### Header CRC-16 Calculation

Header checksum is standard CRC-16 with polynomial `0xA001` (reflection of `0x8005`) initialized to `0x0000`:
- Calculated from byte offset 4 of the block (`type` field) through the end of the header.
- Bytes 0 through 3 (`crc16` and `header_size`) are excluded from the checksum computation.

### MS-DOS 32-bit Timestamp

Standard MS-DOS compact date/time representation:
- Bits 0..4: `Seconds / 2` (2-second granularity, 0..29)
- Bits 5..10: `Minutes` (0..59)
- Bits 11..15: `Hours` (0..23)
- Bits 16..20: `Day of month` (1..31)
- Bits 21..24: `Month` (1..12)
- Bits 25..31: `Year - 1980` (0..127, covering 1980..2107)

### Extraction Path Security & DOS Backslash Normalization

- **Path Storage Format**: Historical DOS/WinACE tools store member filenames using backslash path separators (`\`) in OEM code pages (e.g. `DIR\SUBDIR\FILE.TXT`).
- **POSIX Normalization**: On POSIX/Linux systems, `ace_normalize_path_separators()` translates `\` to `/` upon parsing member headers, seamlessly recreating hierarchical directory structures during extraction.
- **Path Traversal Protection**: Both `ace` and native Linux SFX stubs enforce strict path traversal validation via `ace_is_safe_relpath()`:
  - Continuously tracks directory nesting depth. Any attempt to ascend above the destination root via `../` (or `..\`) is intercepted and safely rejected with:
    ```text
    Security warning: skipping unsafe path traversal '%s'
    ```
  - Rejects leading absolute slashes (`/`, `\`) and DOS drive identifiers (`C:`).
  - Guarantees complete immunity against Zip-Slip vulnerabilities.
- **Unix Path Formatting (`-u`, `--unix`)**: Translates path separators to `/` and normalizes historical uppercase DOS/Windows filenames to lowercase (`README.TXT` $\to$ `readme.txt`, `DIR\SUBDIR\FILE.TXT` $\to$ `dir/subdir/file.txt`), supported both in listing modes and during extraction (`ace x -u`).
- **Bare Output Format (`-1`, `--bare`)**: Emits clean, one-path-per-line listings without headers, dates, or summary banners for straightforward integration into Unix pipelines, scripts, `xargs`, and `grep`.

---

## 3. Bitstream Architecture

### Bit Ordering and 32-bit Accumulator

- Words are refilled from the file as 32-bit Little-Endian integers.
- Bits are consumed and emitted **from MSB (most significant bit) down to LSB (least significant bit)** in the bit accumulator.
- Upon finalizing a compressed block or stream, an alignment to 32 bits (`pad32`) is flushed.

### Huffman Tree Serialization

Each compressed block contains two primary Huffman trees:
1. **Main Tree (`main_tree`)**: 284 symbol codes (0..255 = literals, 256..259 = repeated distances, 260..282 = LZ77 distances, 283 = `TYPECODE`).
2. **Length Tree (`len_tree`)**: 255 symbol codes (LZ77 match copy lengths).

Tree serialization format in the bitstream:
1. `num_widths` (9 bits): Number of active code lengths minus 1.
2. `lower_width` (4 bits): Minimum non-zero code length minus 1 ($W_{min} - 1$).
3. `upper_width` (4 bits): Code length span/range.
4. For each $i \in [0, upper\_width]$: Huffman code width (3 bits) for the secondary code-length tree (`width_tree`).
5. Active code lengths are delta-encoded relative to previous ($ (cur - prev) \pmod{upper\_width} $) with zero-run RLE (symbol $upper\_width$ followed by 4 bits encoding 4 to 19 repeated zeros).
6. Non-zero widths are decoded by: $W = w_{read} + lower\_width$.

### Degenerate Huffman Trees (Single-Symbol Codes)

A subtle edge case in canonical prefix coding arises when a block contains only one unique active symbol (or zero active symbols, such as in empty/sparse residual blocks):
- **Contrast with Standard DEFLATE / RFC 1951**: Many standard algorithms require at least two distinct symbols, or artificially inject a dummy symbol with code length 1 so that the binary tree forms a valid root with two child leaves (`0` and `1`).
- **Marcel Lemke's Implementation in WinACE**: WinACE does **not** synthesize dummy symbols. Instead:
  1. When `used == 1`: The lone active symbol is assigned bit length `1` directly (`widths[sym] = 1`).
  2. The code length table serialization stores this as a single active entry (`num_widths = 0`, `lower_width = 0`, `upper_width = 1`).
  3. **Lookup Table Expansion**: For a decoding table of size $2^{\text{max\_width}}$, all entries whose most significant prefix bit is `0` (the lower half of the table, indices $0 \le i < 2^{\text{max\_width}-1}$) point to this lone symbol with code length 1.
  4. **Bitstream Encoding & Decoding**: The encoder writes a single `0` bit per symbol occurrence. The decoder peeks the stream, matches the lower half of the table, consumes 1 bit, and returns the symbol.
  5. When `used == 0`: No bit lengths are assigned (`widths` remain all 0).

This exact representation is critical for roundtripping audio residual blocks and multi-model streams (`sound.ace` and `best.ace`).

### Lemke's Unstable Quicksort & Canonical Tie-Breaking

To guarantee canonical Huffman trees match official WinACE archives **bit-for-bit**:
- Symbol frequencies are sorted in **descending order** using Lemke's unstable in-place Quicksort partition algorithm.
- Because Quicksort is inherently unstable, symbols sharing identical frequencies remain ordered precisely according to this partition sequence:

```c
static void quicksort_subrange_u32(uint32_t *keys, uint16_t *values, int left, int right)
{
    int new_left = left;
    int new_right = right;
    uint32_t m = keys[right];
    while (1) {
        while (keys[new_left] > m)
            new_left++;
        while (keys[new_right] < m)
            new_right--;
        if (new_left <= new_right) {
            uint32_t tk = keys[new_left]; keys[new_left] = keys[new_right]; keys[new_right] = tk;
            uint16_t tv = values[new_left]; values[new_left] = values[new_right]; values[new_right] = tv;
            new_left++;
            new_right--;
        }
        if (new_left >= new_right)
            break;
    }
    if (left < new_right)
        quicksort_subrange_u32(keys, values, left, new_right);
    if (right > new_left)
        quicksort_subrange_u32(keys, values, new_left, right);
}
```

During bottom-up tree construction:
- The two lowest-weight nodes are selected from the tail of the sorted list.
- Their combined parent node ($W_a + W_b$) is re-inserted by scanning from right to left while $W_{parent} \ge W_{element}$.
- This exact tie-breaking ordering matches WinACE's canonical tree generation identically.

---

## 4. LZ77 Match Finder Engine

Reverse-engineered from routines `0x417300..0x4179E0` of `ACE32.EXE`.

### The Three Hash Tables

Lemke uses **three specialized hash tables segregated by distance reach**:

1. **2-Byte Hash Table (`head2[65536]`)**:
   - Hash key: `in[0] | (in[1] << 8)`
   - Reach: Distances $\le 255$ (`MAXDISTATLEN2`).
   - Minimum match length: 2 bytes.
2. **3-Byte Hash Table (`head3[65536]`)**:
   - Hash key: `(in[1] << 10) + (in[2] << 5) + in[0]`
   - Reach: Distances $\le 8191$ (`MAXDISTATLEN3`).
   - Minimum match length: 3 bytes.
3. **4-Byte Hash Table (`head4[262144]`)**:
   - Hash key: XOR polynomial with cyclic bit rotations:
     $$H = T[in[0]] \oplus \text{rol}(T[in[1]], 4) \oplus \text{rol}(T[in[2]], 8) \oplus \text{rol}(T[in[3]], 12)$$
   - Reach: Distances across the entire sliding dictionary window (up to 4 MiB).
   - Minimum match length: 4 bytes.

### Distance History (Repeat Distances)

Lemke maintains a 4-entry circular distance cache: `hist[4]`.
- When a match reuses a recently used distance:
  - Main symbols `256` through `259` encode cache offsets 0 through 3.
  - **Distance bit cost = 0 bits**.
  - The chosen distance is promoted to the front of the history list (LRU).

### Lazy Matching Evaluation

At input position $i$ with best match of length $L_1$:
- The match finder probes position $i+1$.
- If a match of length $L_2$ is found with $L_2 > L_1 + 1$:
  - The current match is discarded in favor of emitting a literal at $i$ followed by the longer match at $i+1$.
- If $L_2 == L_1$ or $L_2 == L_1 + 1$, Lemke performs bit-cost evaluation (length code bits + distance extra bits) to determine the optimal choice.

### EOF Literal Reservation Rule

A signature rule implemented by Marcel Lemke:
- When remaining input buffer bytes $\le 259$ (`MAXLEN`):
  $$\text{max\_match} = \text{remaining} - 1$$
- **The very last byte of the input stream is never part of an LZ77 match**: it is unconditionally emitted as a literal, providing clean block delimiter termination.

---

## 5. Blocked Modes and ACE 2.0 Preprocessors

### Mode Switching (`TYPECODE 283`)

In blocked mode (`-2` / ACE 2.0):
- Symbol `283` designates an active preprocessor mode transition.
- **Integration Rule**: Symbol `283` is emitted as **token 0 of the first LZ77 data block**, sharing the main Huffman tree and avoiding the ~16-byte overhead of an isolated header block.
- Immediately following Huffman symbol `283`, mode parameters are written directly into the bitstream:
  - `mode` (8 bits): 0 = LZ77, 1 = DELTA, 2 = EXE, 3..6 = SOUND, 7 = PIC.

### DELTA Filter (Mode 1)

Designed for periodic structured data (multichannel audio samples, graphic vectors, tabular records):
- Parameters: `delta_dist` (8 bits, period distance $D$), `delta_len` (17 bits, run length).
- **Transformation**: Data is split into $D$ independent planes. For each plane, each byte is subtracted from its predecessor:
  $$x'[i] = (x[i] - x[i-D]) \pmod{256}$$
- The flattened, repetitive transformed stream is subsequently compressed by the LZ77 engine.
- **Arbitrary Periodicity Detection**:
  - Probes candidate strides $D \in [2, \min(128, N / 4)]$.
  - Periodicity is detected when byte identity rate $\sum [p[i] == p[i-D]] \ge 70\%$ while adjacent byte equality ($D=1$) is low ($< 50\%$, filtering out flat runs).
  - The periodic run length is clamped to $L = (N / D) \times D$.
  - Any remainder $N - L$ bytes are emitted in a trailing plain LZ77 sub-run preceded by a `TYPECODE 283 (mode = 0)` token.
- **Token Stream Accumulation Across Sub-Runs**:
  - Consecutive LZ77-compatible sub-runs (plain LZ77, DELTA, and EXE) do not write separate Huffman blocks.
  - Mode transitions (`TYPECODE 283`) and run tokens are concatenated into a single token array.
  - The combined tokens share the canonical Huffman tree (e.g., 23 delta tokens + 1 switch + 24 literals = 49 tokens in `PIC.BIN`), resulting in an 88-byte payload that matches WinACE bit-for-bit.

### 64 KiB Chunking Constraint for DELTA Blocks

Authentic 32-bit DOS WinACE (`ACE.EXE` running under the PMODE/W extender) and Win32 WinACE enforce a strict **64 KiB ($65,536$ bytes) segment boundary** on DELTA filtering runs:
- **Rationale**: Internal planar de-interleaving and running-sum scratch buffers were originally dimensioned around 16-bit offset indices ($2^{16} = 65,536$) from real-mode DOS heritage.
- **Chunking Rule**: When an uncompressed input stream exceeds 64 KiB and DELTA mode is active:
  1. The stream is partitioned into consecutive segments of maximum length $\le 65,536$ bytes.
  2. Each chunk's length must be an exact multiple of the stride $D = \text{delta\_dist}$:
     $$L_{\text{chunk}} = \left\lfloor \frac{65536}{D} \right\rfloor \times D$$
  3. Each chunk is emitted with its own `TYPECODE 283 (mode = 1, delta_dist = D, delta_len = L_chunk)` header token.
  4. Any final trailing remainder ($< D$ bytes) is emitted in a plain LZ77 sub-run (`mode = 0`).
- **Compatibility**: Archives that emit monolithic DELTA runs exceeding $65,536$ bytes trigger memory corruption or CRC failures when unpacked with genuine DOS WinACE 2.6.

### Solid Dictionary Synchronization Across Filtered Modes

In solid archives (`-s`), the LZ77 sliding window dictionary persists across file member boundaries, allowing Member $N$ to reference strings matched in Member $N-1$:
- **The Filter Inversion Asymmetry**:
  In most conventional archivers (such as RAR or 7-Zip), preprocessors are applied before LZ77 encoding, and the dictionary contains uncompressed original data. In ACE 2.0, however, the preprocessors are coupled directly to the LZ77 streaming engine in an asymmetrical manner:
  - **`ACE_MODE_LZ77_DELTA`**: The LZ77 decoder outputs directly into `lz77.dict`. It writes the **planar delta transformed bytes** directly into the dictionary. The inverse planar permutation and running-sum accumulation are executed *after* data is read out of the LZ77 buffer.
  - **`ACE_MODE_LZ77_EXE`**: The LZ77 decoder writes the **preprocessed bytes** (containing relocated absolute E8/E9 target addresses) directly into `lz77.dict`. The post-processing relocation fixup (`exe_patch`) is applied to the final extracted output buffer *after* LZ77 decoding.
  - **`ACE_MODE_SOUND_*` & `ACE_MODE_PIC`**: These modes do not use the LZ77 decoder to produce their output; their dedicated decoders reconstruct uncompressed samples directly, and then explicitly call `ace_lz77_register` to push the **uncompressed samples** into the LZ77 sliding window.
- **Solid Dictionary Mirroring**:
  To guarantee that solid archives decompress correctly in both authentic WinACE and modern decoders:
  - When compressing Member 1 with DELTA or EXE mode, the encoder must store the **transformed / preprocessed byte stream** into its inter-member history buffer (`hist`).
  - If the encoder were to save the raw uncompressed bytes of Member 1 into `hist`, Member 2's LZ77 match finder would find matches against byte sequences that do not exist in the decoder's dictionary, producing corrupted output and CRC failures.
  - By mirroring the exact transformed stream for DELTA/EXE and the uncompressed stream for SOUND/PIC, solid history remains 100% synchronized across all mode transitions.

### x86 Executable Filter (Mode 2)

Designed for x86 machine code (DOS COM/EXE, Win32 PE):
- Translates relative jump target offsets for `CALL` (`0xE8`) and `JMP` (`0xE9`) instructions into absolute virtual addresses:
  $$\text{dest\_abs} = \text{dest\_rel} + \text{current\_offset}$$
- Mode 0: 16-bit relative addresses (DOS).
- Mode 1: 32-bit relative addresses (Win32 PE).
- Identical function targets become invariant constants, significantly extending LZ77 match lengths.

### SOUND Audio Model (Modes 3-6)

- Modes 3..6: Mono/Stereo 8-bit and 16-bit PCM audio.
- Employs an adaptive linear predictor with dedicated Huffman trees over audio differential residuals.

### PIC 2D Raster Filter (Mode 7)

- Designed for uncompressed image bitmaps (pixel scanlines).
- Parameters: `width` (scanline stride) and `planes` (1 = grayscale/palette, 3 = RGB).
- Applies two-dimensional pixel prediction (subtracting horizontal and vertical neighbors) prior to encoding.

---

## 6. Automatic Mode Selection Heuristic

WinACE does **not** rely on naive magic number checks (such as solely checking `"MZ"` or file extensions).

Reverse-engineered from routines `0x412000..0x412168` of `ACE32.EXE`:

```text
[Input Data Block]
       │
       ├──► Estimated bit-cost Mode 0 (Plain LZ77)   ──► Score S0
       ├──► Estimated bit-cost Mode 1 (DELTA)        ──► Score S1
       └──► Estimated bit-cost Mode 2 (EXE)          ──► Score S2
                               │
                Apply active mode preference ratio:
                   S_active = (S_active * 15) / 16
                               │
            Select Mode with MINIMUM Bit Cost Score
```

### Bit Cost Estimation
For candidate evaluation:
- Lemke simulates expected bitstream cost:
  - Literal frequency code bit costs.
  - Length code bit costs (`0x41139f`).
  - Distance code bit costs (`0x41138b`).
- Mode switch overhead ($\sim 32$ bits for symbol `283` and mode fields) is weighed against projected compression gains.

### The 15/16 Preference Factor
To prevent unnecessary mode toggling for marginal gains:
- The currently active mode receives a $15/16$ score discount (a $6.25\%$ advantage).
- A specialized mode is activated only if its savings comfortably exceed this switching threshold.

### Application to Canonical Test Vectors
- **Small executable stub (`FAKE.EXE`, 256 bytes)**: Two `CALL`/`JMP` instructions save only a few bits, which is insufficient to amortize the ~32-bit mode switch cost. WinACE remains in **plain LZ77** (136 bytes).
- **Periodic delta pattern (`DELTA.BIN`, 512 bytes)**: Planar decomposition reduces 512 bytes into two large repeat runs, saving over 2000 bits. **DELTA mode** is chosen by a large margin (137 bytes, 100% bit-exact).

### Discriminating Audio Predictors from Structured DELTA

Both acoustic waveforms and tabular/columnar binary data exhibit periodicity at short power-of-two intervals (2 bytes for 16-bit mono PCM or 16-bit integer records; 4 bytes for 16-bit stereo PCM or 32-bit columnar telemetry).

However, selecting the wrong preprocessor incurs severe compression penalties:
- **Audio FIR Predictors on Columnar Records**: Adaptive audio models perform linear extrapolation; across discontinuous integer steps or byte-aligned bitfields, this generates high-entropy differential residuals.
- **Pure Byte-Delta Filters on Acoustic Audio**: Subtraction alone leaves significant high-frequency waveform energy that standard LZ77 match finders cannot compress effectively.

To cleanly discriminate between the two representations without relying on superficial file extensions:

Let $S_k$ denote the sum of absolute differences at stride $k$:
$$S_k = \sum_{i=k}^{N-1} |x[i] - x[i-k]|$$

1. **16-bit Stereo PCM Audio (`SOUND_32A`)**:
   - Stereo 16-bit audio interleaves Left and Right 16-bit samples (4-byte period).
   - High correlation occurs across 4-byte sample boundaries:
     $$S_4 \times 3 < S_1 \times 2 \quad \text{and} \quad S_4 \times 3 < S_2 \times 2$$
   - When satisfied, `SOUND_32A` is chosen.

2. **16-bit Mono PCM Audio (`SOUND_16`) vs. Structured 16-bit DELTA ($D=2$)**:
   - For audio waveforms, 2-byte differences are smaller than 1-byte differences ($S_2 \times 3 < S_1 \times 2$), but natural acoustic dynamic variation ensures that $S_2$ remains within an order of magnitude of $S_1$:
     $$S_2 \times 8 > S_1$$
   - When $S_2 \times 8 \le S_1$, the 2-byte stride difference is drastically smaller than adjacent byte variance. This signature indicates **structured columnar integer data** (such as fixed-point telemetry, record tables with constant high-order bytes, or little-endian integer counters). In this regime, planar separation via `DELTA (D=2)` isolates high and low bytes into uniform planes that compress to nearly zero overhead, far outperforming audio prediction models.

3. **Arbitrary Periodic DELTA ($D \in [2, 128]$)**:
   - Probes candidate strides $D \in [2, \min(128, N/4)]$.
   - Evaluates autocorrelation identity match rate:
     $$\text{Matches}(D) = \sum_{i=D}^{N-1} [x[i] == x[i-D]]$$
   - If $\text{Matches}(D) \ge 0.70 \times (N - D)$ while $\text{Matches}(1) < 0.50 \times (N - 1)$, the stream is identified as structured periodic data with period $D$.

---

## 7. Cross-Validation and Empirical Verification

### DOSBox PMODE/W WinACE 2.6 Interoperability

Reverse engineering was validated bidirectionally against Marcel Lemke's authentic 32-bit DOS binary `ACE.EXE` v2.6 executing under DOSBox:
- **PMODE/W DOS Extender Environment**: The DOS binary is packaged with the PMODE/W DOS extender (`pmodew.exe`). It executes in 32-bit protected mode under DPMI/VCPI.
- **Disk Paging and Scratch Files (`$$$ACE0.TMP`)**: In constrained DOS memory environments, `ACE.EXE` automatically pages solid compression dictionaries and intermediate structures to temporary disk files named `$$$ACE0.TMP` when compressing archives with 4 or more large members. For automated DOSBox batch verification, solid sets are grouped into $\le 3$-4 members per volume (`DOSGRP1.ACE`, `DOSGRP2.ACE`) to avoid DOS filesystem locking conflicts.
- **Bidirectional Verification**:
  1. Linux `ace a` creates archives across all compression modes, options, and solid configurations $\to$ tested by `ACE.EXE T -y` in DOSBox $\to$ all pass with `CRC OK`.
  2. Authentic `ACE.EXE A` in DOSBox creates archives $\to$ extracted and tested by our Linux `ace` $\to$ all pass with identical SHA-256 digests.

### Real-World Corpus Validation Benchmark

The complete interoperability matrix is codified in `tests/test_real_world.py`, testing realistic corpora (executables, audio, python source code, structured databases, text manuals, and raster images):
- **100% Bit-Exact Recovery**: All members verify with identical SHA-256 hashes upon decompression.
- **High Compression Ratios**: Up to $99.6\%$ compression on periodic telemetry via DELTA mode, $44.0\%$ on stereo audio via SOUND mode, and $31.8\%$ on uncompressed RGB images via PIC mode.
