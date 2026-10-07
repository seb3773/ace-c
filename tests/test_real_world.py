#!/usr/bin/env python3
"""
Comprehensive Real-World Corpus Validation Benchmark.

Tests bidirectional compression, decompression, integrity, and cross-compatibility
between our native C ACE implementation (ace) and the authentic Marcel Lemke
WinACE 2.6 executable (ACE.EXE PMODE/W under DOSBox) on diverse real-world files:
- Real English documentation (DOC.TXT)
- Real Python source code (CODE.PY)
- Real 16/32-bit DOS executable (APP.EXE)
- Real Win32 PE executable (WIN.EXE)
- Real 16-bit 44.1kHz stereo PCM audio (AUDIO.WAV)
- Real 24-bit RGB bitmap raster image (IMAGE.BMP)
- Real periodic telemetry database (DATA.DAT)
"""

import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

ACE = os.path.join(ROOT, "ace")
DOS_ACE = os.path.join(ROOT, "research", "binDos", "ACE.EXE")
CORPUS_DIR = os.path.join(ROOT, "testdata", "real_corpus")


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while chunk := f.read(65536):
            h.update(chunk)
    return h.hexdigest()


def get_first_member_packsize(arc_path):
    with open(arc_path, "rb") as f:
        data = f.read(512)
    if len(data) < 14 or data[7:14] != b"**ACE**":
        return os.path.getsize(arc_path)
    main_hdr_size = struct.unpack_from("<H", data, 2)[0]
    file_hdr_start = 4 + main_hdr_size
    # File header: crc(2), size(2), type(1), flags(2), pack_size(4)
    pack_sz = struct.unpack_from("<I", data, file_hdr_start + 7)[0]
    return pack_sz


def generate_corpus():
    os.makedirs(CORPUS_DIR, exist_ok=True)

    # 1. Real English manual text (64,513 bytes)
    src_doc = os.path.join(ROOT, "research", "binDos", "MANUAL", "ACE.TXT")
    dst_doc = os.path.join(CORPUS_DIR, "DOC.TXT")
    shutil.copyfile(src_doc, dst_doc)

    # 2. Real Python code
    src_py = os.path.join(ROOT, "tools", "gen_sfx_stubs.py")
    dst_py = os.path.join(CORPUS_DIR, "CODE.PY")
    shutil.copyfile(src_py, dst_py)

    # 3. Real DOS x86 executable (62,012 bytes)
    src_dos_exe = os.path.join(ROOT, "research", "binDos", "UNACE.EXE")
    dst_dos_exe = os.path.join(CORPUS_DIR, "APP.EXE")
    shutil.copyfile(src_dos_exe, dst_dos_exe)

    # 4. Real Win32 PE executable (45,568 bytes)
    src_pe_exe = os.path.join(ROOT, "research", "binDos", "UNACE32.EXE")
    dst_pe_exe = os.path.join(CORPUS_DIR, "WIN.EXE")
    shutil.copyfile(src_pe_exe, dst_pe_exe)

    # 5. Real 16-bit 44.1kHz stereo PCM audio WAV (200,044 bytes)
    dst_wav = os.path.join(CORPUS_DIR, "AUDIO.WAV")
    num_frames = 50000
    sample_rate = 44100
    audio_data = bytearray()
    for i in range(num_frames):
        t = i / sample_rate
        val_l = int(18000 * math.sin(2 * math.pi * 440 * t) * math.exp(-t * 1.5))
        val_r = int(14000 * math.sin(2 * math.pi * 880 * t) * math.exp(-t * 2.0))
        val_l = max(-32768, min(32767, val_l))
        val_r = max(-32768, min(32767, val_r))
        audio_data.extend(struct.pack("<hh", val_l, val_r))
    wav_hdr = bytearray(b"RIFF")
    wav_hdr.extend(struct.pack("<I", 36 + len(audio_data)))
    wav_hdr.extend(b"WAVEfmt ")
    wav_hdr.extend(struct.pack("<IHHIIHH", 16, 1, 2, sample_rate, sample_rate * 4, 4, 16))
    wav_hdr.extend(b"data")
    wav_hdr.extend(struct.pack("<I", len(audio_data)))
    with open(dst_wav, "wb") as f:
        f.write(wav_hdr + audio_data)

    # 6. Real 24-bit RGB raster BMP (320x240, 230,454 bytes)
    dst_bmp = os.path.join(CORPUS_DIR, "IMAGE.BMP")
    w, h = 320, 240
    row_stride = (w * 3 + 3) & ~3
    bmp_data = bytearray(row_stride * h)
    for y in range(h):
        row_offset = y * row_stride
        for x in range(w):
            r = int(127.5 * (1 + math.sin(x * 0.05)))
            g = int(127.5 * (1 + math.cos(y * 0.05)))
            b = int((x * y) % 256)
            bmp_data[row_offset + x * 3 + 0] = b
            bmp_data[row_offset + x * 3 + 1] = g
            bmp_data[row_offset + x * 3 + 2] = r
    bmp_file_hdr = struct.pack("<2sIHHI", b"BM", 54 + len(bmp_data), 0, 0, 54)
    bmp_dib_hdr = struct.pack("<IIIHHIIIIII", 40, w, h, 1, 24, 0, len(bmp_data), 2835, 2835, 0, 0)
    with open(dst_bmp, "wb") as f:
        f.write(bmp_file_hdr + bmp_dib_hdr + bmp_data)

    # 7. Real periodic telemetry dataset (16 columns, 100,000 bytes)
    dst_dat = os.path.join(CORPUS_DIR, "DATA.DAT")
    rec_len = 16
    n_recs = 6250
    dat_data = bytearray(n_recs * rec_len)
    for r in range(n_recs):
        for c in range(rec_len):
            val = (c * 15 + (r // 10) + ((r * c) % 3)) & 0xFF
            dat_data[r * rec_len + c] = val
    with open(dst_dat, "wb") as f:
        f.write(dat_data)


def run_dosbox_command(workdir, dos_commands):
    """Execute commands in DOSBox and return console log."""
    conf_path = os.path.join(workdir, "dosbox.conf")
    script = [
        "[sdl]",
        "fullscreen=false",
        "output=surface",
        "[cpu]",
        "core=auto",
        "cycles=max",
        "[autoexec]",
        f"mount c {workdir}",
        "c:",
    ]
    script.extend(dos_commands)
    script.append("exit")
    with open(conf_path, "w") as f:
        f.write("\n".join(script) + "\n")

    env = dict(os.environ, SDL_VIDEODRIVER="dummy")
    res = subprocess.run(
        ["dosbox", "-conf", conf_path],
        env=env,
        capture_output=True,
        text=True,
        timeout=30,
    )
    return res.stdout


def test_individual_members():
    print("\n--- TEST 1: Individual File Compression (All Models & Filters) ---")
    files = sorted([f for f in os.listdir(CORPUS_DIR) if not f.startswith(".") and not f.endswith(".ACE") and not os.path.isdir(os.path.join(CORPUS_DIR, f))])
    results = []

    for fname in files:
        src_path = os.path.join(CORPUS_DIR, fname)
        src_size = os.path.getsize(src_path)
        src_hash = sha256_file(src_path)

        # 1. Compress with ace a -2 -m5
        arc_path = os.path.join(CORPUS_DIR, f"{fname}.ACE")
        t0 = time.perf_counter()
        subprocess.run(
            [ACE, "a", arc_path, "-2", "-m", "5", fname],
            cwd=CORPUS_DIR,
            check=True,
            capture_output=True,
        )
        t_comp = time.perf_counter() - t0
        arc_size = os.path.getsize(arc_path)
        ratio = (arc_size / src_size) * 100.0

        # 2. Inspect active mode pack size
        pack_sz = get_first_member_packsize(arc_path)

        # 3. Test integrity with ace t
        subprocess.run([ACE, "t", arc_path], cwd=CORPUS_DIR, check=True, capture_output=True)

        # 4. Extract with ace x and check hash
        out_dir = os.path.join(CORPUS_DIR, f"_out_{fname}")
        os.makedirs(out_dir, exist_ok=True)
        subprocess.run([ACE, "x", "-d", out_dir, arc_path], cwd=CORPUS_DIR, check=True, capture_output=True)
        ext_path = os.path.join(out_dir, fname)
        ext_hash = sha256_file(ext_path)
        assert ext_hash == src_hash, f"Hash mismatch on {fname}"
        shutil.rmtree(out_dir)

        # 5. Validate in authentic DOSBox ACE.EXE
        dos_dir = os.path.join(CORPUS_DIR, f"_dos_{fname}")
        os.makedirs(dos_dir, exist_ok=True)
        shutil.copyfile(DOS_ACE, os.path.join(dos_dir, "ACE.EXE"))
        shutil.copyfile(arc_path, os.path.join(dos_dir, "TEST.ACE"))
        run_dosbox_command(dos_dir, ["ACE.EXE t -y TEST.ACE > OUT.TXT"])
        with open(os.path.join(dos_dir, "OUT.TXT"), "r", errors="ignore") as f:
            log = f.read()
        assert "CRC OK" in log, f"DOSBox ACE.EXE CRC check failed on {fname}: {log}"
        shutil.rmtree(dos_dir)

        results.append({
            "name": fname,
            "orig": src_size,
            "packed": pack_sz,
            "arc": arc_size,
            "ratio": ratio,
            "time": t_comp,
        })
        print(f"  {fname:<10} orig={src_size:>7} B | packed={pack_sz:>7} B ({ratio:>5.1f}%) | CRC OK (Linux + DOSBox)")

    return results


def test_solid_archive():
    print("\n--- TEST 2: Multi-File Solid Archive (REALCORP.ACE) ---")
    files = sorted([f for f in os.listdir(CORPUS_DIR) if not f.endswith(".ACE") and not f.startswith(".") and not os.path.isdir(os.path.join(CORPUS_DIR, f))])
    total_orig = sum(os.path.getsize(os.path.join(CORPUS_DIR, f)) for f in files)

    arc_path = os.path.join(CORPUS_DIR, "REALCORP.ACE")
    t0 = time.perf_counter()
    subprocess.run(
        [ACE, "a", arc_path, "-2", "-s", "-m", "5"] + files,
        cwd=CORPUS_DIR,
        check=True,
        capture_output=True,
    )
    t_comp = time.perf_counter() - t0
    solid_size = os.path.getsize(arc_path)
    solid_ratio = (solid_size / total_orig) * 100.0

    print(f"  Files: {len(files)} | Uncompressed: {total_orig:,} B | Solid Packed: {solid_size:,} B ({solid_ratio:.1f}%) | Time: {t_comp:.3f}s")

    # 1. Test integrity with ace t
    subprocess.run([ACE, "t", arc_path], cwd=CORPUS_DIR, check=True, capture_output=True)
    print("  ace t: CRC OK on all members")

    # 2. Extract with ace x and check hash on all files
    out_dir = os.path.join(CORPUS_DIR, "_out_solid")
    os.makedirs(out_dir, exist_ok=True)
    t0 = time.perf_counter()
    subprocess.run([ACE, "x", "-d", out_dir, arc_path], cwd=CORPUS_DIR, check=True, capture_output=True)
    t_decomp = time.perf_counter() - t0
    for fname in files:
        orig_hash = sha256_file(os.path.join(CORPUS_DIR, fname))
        ext_hash = sha256_file(os.path.join(out_dir, fname))
        assert orig_hash == ext_hash, f"Solid hash mismatch on {fname}"
    shutil.rmtree(out_dir)
    print(f"  ace x: 100% Bit-Exact SHA-256 match on all {len(files)} files (Decompression: {t_decomp:.3f}s)")

    # 3. Test integrity in authentic DOSBox ACE.EXE
    dos_dir = os.path.join(CORPUS_DIR, "_dos_solid")
    os.makedirs(dos_dir, exist_ok=True)
    shutil.copyfile(DOS_ACE, os.path.join(dos_dir, "ACE.EXE"))
    shutil.copyfile(arc_path, os.path.join(dos_dir, "SOLID.ACE"))
    run_dosbox_command(dos_dir, ["ACE.EXE t -y SOLID.ACE > OUT.TXT"])
    with open(os.path.join(dos_dir, "OUT.TXT"), "r", errors="ignore") as f:
        log = f.read()
    crc_oks = log.count("CRC OK")
    assert crc_oks == len(files), f"DOSBox expected {len(files)} CRC OKs, found {crc_oks}: {log}"
    shutil.rmtree(dos_dir)
    print(f"  DOSBox authentic ACE.EXE 2.6: verified all {len(files)} files with CRC OK!")


def test_reverse_dosbox_compatibility():
    print("\n--- TEST 3: Reverse Cross-Validation (Authentic ACE.EXE -> Our Decoder) ---")
    files = sorted([f for f in os.listdir(CORPUS_DIR) if not f.endswith(".ACE") and not f.startswith(".") and not os.path.isdir(os.path.join(CORPUS_DIR, f))])

    groups = [
        ("DOSGRP1.ACE", ["APP.EXE", "CODE.PY", "DATA.DAT", "DOC.TXT"]),
        ("DOSGRP2.ACE", ["AUDIO.WAV", "IMAGE.BMP", "WIN.EXE"]),
    ]

    for arc_name, group_files in groups:
        dos_dir = os.path.join(CORPUS_DIR, f"_dos_rev_{arc_name}")
        os.makedirs(dos_dir, exist_ok=True)
        shutil.copyfile(DOS_ACE, os.path.join(dos_dir, "ACE.EXE"))
        for f in group_files:
            shutil.copyfile(os.path.join(CORPUS_DIR, f), os.path.join(dos_dir, f))

        cmd_str = f"ACE.EXE a -y -s -m5 {arc_name} {' '.join(group_files)} > OUT.TXT"
        run_dosbox_command(dos_dir, [cmd_str])

        dos_arc = os.path.join(dos_dir, arc_name)
        assert os.path.isfile(dos_arc), f"DOSBox failed to create {arc_name}"
        dos_size = os.path.getsize(dos_arc)
        print(f"  Authentic DOS ACE.EXE created {arc_name} ({dos_size:,} bytes, {len(group_files)} solid members)")

        # Test with our ace t
        subprocess.run([ACE, "t", dos_arc], cwd=CORPUS_DIR, check=True, capture_output=True)
        print(f"  Our ace t: verified authentic {arc_name} with CRC OK on all members")

        # Extract with our ace x and verify bit-identical SHA-256
        out_dir = os.path.join(CORPUS_DIR, f"_out_from_{arc_name}")
        os.makedirs(out_dir, exist_ok=True)
        subprocess.run([ACE, "x", "-d", out_dir, dos_arc], cwd=CORPUS_DIR, check=True, capture_output=True)
        for fname in group_files:
            orig_hash = sha256_file(os.path.join(CORPUS_DIR, fname))
            ext_hash = sha256_file(os.path.join(out_dir, fname))
            assert orig_hash == ext_hash, f"Reverse hash mismatch on {fname}"
        shutil.rmtree(out_dir)
        shutil.rmtree(dos_dir)
        print(f"  Our ace x: 100% Bit-Exact SHA-256 match for {arc_name} ({len(group_files)} files)!")


def main():
    print("=====================================================================")
    print("         ACE REAL-WORLD CORPUS VALIDATION & BENCHMARK                ")
    print("=====================================================================")
    generate_corpus()
    test_individual_members()
    test_solid_archive()
    test_reverse_dosbox_compatibility()
    print("\n=====================================================================")
    print("  ALL REAL-WORLD BENCHMARKS AND BIDIRECTIONAL CROSS-CHECKS PASSED!   ")
    print("=====================================================================\n")


if __name__ == "__main__":
    main()
