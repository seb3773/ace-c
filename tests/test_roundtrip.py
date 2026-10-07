#!/usr/bin/env python3
"""Comprehensive roundtrip tests for the ACE compression and extraction engine."""
import datetime
import glob
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def mkace(*args):
    cmd = [os.path.join(ROOT, "ace"), "a", *args]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"ace a failed: {' '.join(args)}\n{r.stderr}")


def unace_test(archive, pwd=None):
    cmd = [os.path.join(ROOT, "ace"), "t", archive]
    if pwd is not None:
        cmd[2:2] = ["-p", pwd]
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"ace t failed: {archive}\n{r.stdout}{r.stderr}")


def unace_extract_bytes(archive, pwd=None):
    """Extract a single-member archive with ace x and return the file bytes."""
    dest = tempfile.mkdtemp(prefix="_rt_", dir=os.path.join(ROOT, "testdata"))
    try:
        cmd = [os.path.join(ROOT, "ace"), "x", "-d", dest, archive]
        if pwd is not None:
            cmd[2:2] = ["-p", pwd]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"ace x failed: {archive}\n{r.stdout}{r.stderr}")
        names = [n for n in os.listdir(dest) if os.path.isfile(os.path.join(dest, n))]
        if len(names) != 1:
            raise SystemExit(f"ace x: expected 1 file, got {names}")
        with open(os.path.join(dest, names[0]), "rb") as fh:
            return fh.read()
    finally:
        shutil.rmtree(dest, ignore_errors=True)


def check_archive_member(archive, expected, pwd=None):
    unace_test(archive, pwd=pwd)
    got = unace_extract_bytes(archive, pwd=pwd)
    if got != expected:
        raise SystemExit(f"data mismatch for {archive}: got {len(got)} want {len(expected)}")


def check_archive_multi(archive, expected_list):
    unace_test(archive)
    dest = tempfile.mkdtemp(prefix="_rtm_", dir=os.path.join(ROOT, "testdata"))
    try:
        cmd = [os.path.join(ROOT, "ace"), "x", "-d", dest, archive]
        r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"ace x failed: {archive}\n{r.stderr}")
        files = sorted(os.listdir(dest))
        if len(files) != len(expected_list):
            raise SystemExit(f"expected {len(expected_list)} files, got {len(files)}")
        for fname, exp in zip(files, expected_list):
            with open(os.path.join(dest, fname), "rb") as fh:
                if fh.read() != exp:
                    raise SystemExit(f"multi data mismatch for {fname}")
    finally:
        shutil.rmtree(dest, ignore_errors=True)


def _put(payload):
    return struct.pack("<HH", (zlib.crc32(payload, 0) ^ 0xFFFFFFFF) & 0xFFFF, len(payload)) + payload


def write_bytes(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def gen_path_store_ace(path, name, data, when):
    """Write a minimal ACE 2.0 stored archive holding one path-bearing member."""
    def ace_crc32(b):
        return zlib.crc32(b, 0) ^ 0xFFFFFFFF

    def put(payload):
        return struct.pack("<HH", ace_crc32(payload) & 0xFFFF, len(payload)) + payload

    dt = (((when.year - 1980) << 25) | (when.month << 21) | (when.day << 16)
          | (when.hour << 11) | (when.minute << 5) | (when.second // 2))
    name_b = name if isinstance(name, (bytes, bytearray)) else name.encode("utf-8")
    main = struct.pack("<BH", 0, 1 << 8) + b"**ACE**"
    main += struct.pack("<BBBB", 20, 20, 12, 0) + struct.pack("<L", dt)
    main += b"\x00" * 8 + b"\x00"
    fb = struct.pack("<BH", 1, 1) + struct.pack("<LL", len(data), len(data))
    fb += struct.pack("<LLLBBHHH", dt, 0x20, ace_crc32(data), 0, 0, 0, 0, len(name_b))
    fb += name_b
    write_bytes(path, put(main) + put(fb) + data)


def run_ace(*args):
    cmd = [os.path.join(ROOT, "ace"), *args]
    return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)


def unified_cli_check():
    """The single `ace` binary: create/list/verbose/test/dump/extract."""
    import tempfile

    td = os.path.join(ROOT, "testdata")
    data = b"unified ace frontend payload\n" * 8
    src = os.path.join(td, "_ucli.txt")
    arch = os.path.join(td, "_ucli.ace")
    write_bytes(src, data)

    r = run_ace("a", arch, "-z", "testdata/_ucli.txt")
    if r.returncode != 0 or not os.path.exists(arch):
        raise SystemExit(f"ace a failed\n{r.stdout}{r.stderr}")

    r = run_ace("l", arch)
    if r.returncode != 0 or "_ucli.txt" not in r.stdout:
        raise SystemExit(f"ace l mismatch\n{r.stdout}")
    r = run_ace("v", arch)
    if r.returncode != 0 or "archive" not in r.stdout:
        raise SystemExit(f"ace v mismatch\n{r.stdout}")
    r = run_ace("t", arch)
    if r.returncode != 0:
        raise SystemExit(f"ace t failed\n{r.stdout}{r.stderr}")
    r = run_ace("d", arch)
    if "FILE" not in r.stdout or "MAIN" not in r.stdout:
        raise SystemExit(f"ace d mismatch\n{r.stdout}")

    dest = tempfile.mkdtemp(prefix="_ucli_", dir=td)
    try:
        r = run_ace("x", "-d", dest, arch)
        if r.returncode != 0:
            raise SystemExit(f"ace x failed\n{r.stdout}{r.stderr}")
        got = [os.path.join(dest, n) for n in os.listdir(dest)]
        if len(got) != 1 or open(got[0], "rb").read() != data:
            raise SystemExit("ace x data mismatch")
    finally:
        shutil.rmtree(dest, ignore_errors=True)

    dest_e = tempfile.mkdtemp(prefix="_uclie_", dir=td)
    try:
        r = run_ace("e", "-d", dest_e, arch)
        if r.returncode != 0:
            raise SystemExit(f"ace e failed\n{r.stdout}{r.stderr}")
        got = [os.path.join(dest_e, n) for n in os.listdir(dest_e)]
        if len(got) != 1 or open(got[0], "rb").read() != data:
            raise SystemExit("ace e data mismatch")
    finally:
        shutil.rmtree(dest_e, ignore_errors=True)

    # Extractor agrees on the ace-created archive.
    check_archive_member(arch, data)

    os.remove(src)
    os.remove(arch)
    print("ok ace unified CLI (a/x/e/l/v/t/d)")


def cli_extras_checks():
    """Exercise ace CLI decoder extras: wildcards, -j (junk path), -k (mtime)."""
    td = os.path.join(ROOT, "testdata")
    dest = os.path.join(td, "_cli")

    # Case-insensitive wildcard selection on the WinACE-style solid archive.
    solid = os.path.join(td, "rt_solid.ace")
    r = run_ace("l", solid, "*cat1*")
    if "1 file(s)" not in r.stdout or "rt_cat2" in r.stdout:
        raise SystemExit(f"wildcard list mismatch:\n{r.stdout}")
    r = run_ace("t", solid, "*CAT2*")
    if r.returncode != 0 or "rt_cat2.txt: OK" not in r.stdout:
        raise SystemExit(f"case-insensitive wildcard test mismatch:\n{r.stdout}{r.stderr}")

    # Path-bearing stored member: -j flattens, default preserves directories.
    junk = os.path.join(td, "rt_junkpath.ace")
    payload = b"junk-path member\n"
    gen_path_store_ace(junk, "sub/dir/file.txt", payload,
                       datetime.datetime(2020, 5, 6, 7, 8, 8))
    for tag, extra in (("keeppath", []), ("junk", ["-j"])):
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest)
        r = run_ace("x", *extra, "-d", dest, junk)
        if r.returncode != 0:
            raise SystemExit(f"{tag} extract failed:\n{r.stdout}{r.stderr}")
        files = sorted(os.path.relpath(os.path.join(dp, fn), dest)
                       for dp, _, fns in os.walk(dest) for fn in fns)
        want = [os.path.join(*(["sub", "dir", "file.txt"] if tag == "keeppath" else ["file.txt"]))]
        if files != want:
            raise SystemExit(f"{tag}: got {files} want {want}")

    # -k restores the stored modification time.
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    r = run_ace("x", "-j", "-k", "-d", dest, junk)
    if r.returncode != 0:
        raise SystemExit(f"-k extract failed:\n{r.stdout}{r.stderr}")
    mtime = datetime.date.fromtimestamp(os.path.getmtime(os.path.join(dest, "file.txt")))
    if (mtime.year, mtime.month, mtime.day) != (2020, 5, 6):
        raise SystemExit(f"-k mtime not restored: {mtime}")
    shutil.rmtree(dest, ignore_errors=True)
    os.remove(junk)
    print("ok cli_extras (wildcards, -j, -k)")


def oem_cross_check():
    """Cross-validate unace --oem 850/437 name decoding.

    Uses byte 0x9E, which differs between the two code pages (cp850 -> U+00D8
    'O-with-stroke', cp437 -> U+00A5 yen), to exercise the transcode tables.
    """
    td = os.path.join(ROOT, "testdata")
    dest = os.path.join(td, "_oem")
    raw = b"caf\x82\x9e.txt"  # e-acute + codepage-distincting byte
    for cp, expected in (("850", "cp850"), ("437", "cp437")):
        ace = os.path.join(td, "rt_oem.ace")
        gen_path_store_ace(ace, raw, b"oem name test\n",
                           datetime.datetime(2024, 3, 4, 5, 6, 6))
        ref = raw.decode(expected)
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest)
        r = run_ace("x", "--oem", cp, "-d", dest, ace)
        if r.returncode != 0:
            raise SystemExit(f"--oem {cp} extract failed:\n{r.stdout}{r.stderr}")
        got = os.listdir(dest)
        if got != [ref]:
            raise SystemExit(f"--oem {cp}: got {got} want {[ref]}")
        os.remove(ace)
    shutil.rmtree(dest, ignore_errors=True)
    print("ok oem (cp850/cp437)")


def multivol_encode_check():
    """Validate mkace -V multi-volume output."""
    td = os.path.join(ROOT, "testdata")
    src = os.path.join(td, "rt_mvsrc.txt")
    arch = os.path.join(td, "rt_mvenc.ace")
    data = b"multi-volume ACE encoder payload 0123456789 \n" * 600  # ~21 KiB, compressible
    write_bytes(src, data)
    mkace("-z", "-V", "64", "-o", arch, src)
    if not os.path.exists(os.path.join(td, "rt_mvenc.c00")):
        raise SystemExit("mkace -V produced no follow-on volume (.c00)")
    unace_test(arch)
    if unace_extract_bytes(arch) != data:
        raise SystemExit("unace multi-volume extract mismatch (rt_mvenc)")
    # Clean every generated volume (.ace, .c00..cNN) whatever the split count.
    for p in glob.glob(os.path.join(td, "rt_mvenc.*")):
        os.remove(p)
    os.remove(src)
    print("ok mkace -V multivolume (unace)")


def encoder_modes_check():
    """Verify mkace forced EXE and DELTA conversion modes."""
    import struct
    td = os.path.join(ROOT, "testdata")
    # DELTA: 16-bit little-endian ramp with strong interleave correlation.
    delta = struct.pack("<" + "h" * 400, *[(i * 3) & 0x7FFF for i in range(400)])
    # EXE: MZ header plus call/jmp ops.
    exe = b"MZ" + bytes(range(256)) * 8 + b"\xE8\x10\x00" + b"\x90" * 300
    for flag, name, data in (("-dl", "rt_delta.bin", delta), ("-xe", "rt_exe.bin", exe)):
        src = os.path.join(td, name)
        arch = os.path.join(td, "rt_mode.ace")
        write_bytes(src, data)
        mkace("-2", flag, "-o", arch, src)
        unace_test(arch)
        check_archive_member(arch, data)
        os.remove(src)
        os.remove(arch)
    print("ok mkace -xe/-dl forced modes (unace)")


def attrs_preserve_check():
    """Verify mkace writes DOS attrs/mtime and ace x -k restores them."""
    td = os.path.join(ROOT, "testdata")
    src = os.path.join(td, "rt_attr.txt")
    arch = os.path.join(td, "rt_attr.ace")
    data = b"attributes and mtime preservation roundtrip\n"
    write_bytes(src, data)
    # DOS timestamps have 2-second resolution, so pick an even second.
    when = datetime.datetime(2021, 3, 4, 5, 6, 8)
    ts = when.timestamp()
    os.utime(src, (ts, ts))
    os.chmod(src, 0o444)
    mkace("-o", arch, src)
    unace_test(arch)
    dest = tempfile.mkdtemp(prefix="_rta_", dir=td)
    try:
        r = subprocess.run([os.path.join(ROOT, "ace"), "x", "-k", "-d", dest, arch],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"ace x -k failed:\n{r.stderr}")
        ext_file = os.path.join(dest, "rt_attr.txt")
        st = os.stat(ext_file)
        if int(st.st_mtime) != int(ts):
            raise SystemExit(f"rt_attr: mtime {int(st.st_mtime)} want {int(ts)}")
        if open(ext_file, "rb").read() != data:
            raise SystemExit("rt_attr: data mismatch")
    finally:
        shutil.rmtree(dest, ignore_errors=True)
    os.remove(src)
    os.remove(arch)
    print("ok mkace attrs/mtime (unace)")


def path_preserve_check():
    """Verify mkace -A stores ACE paths and restores them on extract."""
    td = os.path.join(ROOT, "testdata")
    rel = os.path.join("_np", "deep", "nested")
    dirpath = os.path.join(td, rel)
    os.makedirs(dirpath, exist_ok=True)
    fname = "long_filename_test.txt"
    src = os.path.join(dirpath, fname)
    data = b"long path / LFN preservation roundtrip\n"
    write_bytes(src, data)
    argpath = os.path.join("testdata", rel, fname)
    arch_rel = os.path.join("testdata", "rt_path.ace")
    arch = os.path.join(td, "rt_path.ace")
    mkace("-A", "-z", "-o", arch_rel, argpath)
    expected = argpath.replace("\\", "/")
    r = run_ace("l", "-1", arch)
    if expected not in r.stdout.replace("\\", "/"):
        raise SystemExit(f"rt_path: {expected!r} not in ace l -1 output:\n{r.stdout}")
    dest = tempfile.mkdtemp(prefix="_rtp_", dir=td)
    try:
        r = run_ace("x", "-d", dest, arch)
        if r.returncode != 0:
            raise SystemExit(f"ace x failed:\n{r.stderr}")
        ext_path = os.path.join(dest, *argpath.split("/"))
        if not os.path.exists(ext_path) or open(ext_path, "rb").read() != data:
            raise SystemExit("rt_path: extract mismatch")
    finally:
        shutil.rmtree(dest, ignore_errors=True)
    os.remove(src)
    os.remove(arch)
    shutil.rmtree(os.path.join(td, "_np"), ignore_errors=True)
    print("ok mkace -A paths/LFN (unace)")


def comment_modes_check():
    """Verify member + main comments roundtrip in blocked (-2) mode."""
    td = os.path.join(ROOT, "testdata")
    src = os.path.join(td, "rt_cmt.txt")
    arch = os.path.join(td, "rt_cmt.ace")
    data = b"blocked member comment regression payload\n" * 8
    main_cm = "MAIN BLOCKED CM"
    file_cm = "FILE BLOCKED CM"
    write_bytes(src, data)
    mkace("-2", "-cm", main_cm, "-cf", file_cm, "-o", arch, src)
    r_v = run_ace("v", arch)
    if main_cm not in r_v.stdout:
        raise SystemExit(f"rt_cmt: main comment {main_cm!r} missing in ace v:\n{r_v.stdout}")
    r_d = run_ace("d", arch)
    if "cm=15" not in r_d.stdout and "cm=" not in r_d.stdout:
        raise SystemExit(f"rt_cmt: member comment size missing in ace d:\n{r_d.stdout}")
    check_archive_member(arch, data)
    os.remove(src)
    os.remove(arch)
    print("ok mkace -2 comments (unace)")


def dir_recursive_check():
    """Verify mkace stores directories recursively (tree + empty dir)."""
    import tempfile
    td = os.path.join(ROOT, "testdata")
    base = os.path.join(td, "rt_tree")
    arch = os.path.join(td, "rt_treeenc.ace")
    shutil.rmtree(base, ignore_errors=True)
    os.makedirs(os.path.join(base, "sub", "deep"))
    os.makedirs(os.path.join(base, "empty"))
    tree = {
        "root.txt": b"root file contents\n",
        "sub/a.txt": b"alpha aaa\n",
        "sub/deep/b.txt": b"bravo bbb data payload\n",
    }
    for rel, data in tree.items():
        write_bytes(os.path.join(base, *rel.split("/")), data)
    mkace("-z", "-o", arch, base)
    unace_test(arch)
    dest = tempfile.mkdtemp(prefix="_rtd_", dir=td)
    try:
        r = subprocess.run([os.path.join(ROOT, "ace"), "x", "-k", "-d", dest, arch],
                           cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"ace x tree failed:\n{r.stdout}{r.stderr}")
        rootdir = os.path.join(dest, "rt_tree")
        for rel, data in tree.items():
            got = open(os.path.join(rootdir, *rel.split("/")), "rb").read()
            if got != data:
                raise SystemExit(f"tree file mismatch: {rel}")
        if not os.path.isdir(os.path.join(rootdir, "empty")):
            raise SystemExit("empty directory not preserved on extract")
    finally:
        shutil.rmtree(dest, ignore_errors=True)
    os.remove(arch)
    shutil.rmtree(base, ignore_errors=True)
    print("ok mkace recursive dir (unace)")


def datetime_bounds_check():
    """Cover DOS datetime extremes: encoder clamps, decoder tolerates old/corrupt fields."""
    td = os.path.join(ROOT, "testdata")
    src = os.path.join(td, "rt_dt.txt")
    arch = os.path.join(td, "rt_dt.ace")
    data = b"datetime bounds payload\n"

    def expected_clamp(y, mo, d, h, mi, s):
        # Mirrors mkace dos_from_time: 7-bit year offset clamped to [0, 127].
        off = y - 1980
        off = 0 if off < 0 else (127 if off > 127 else off)
        return datetime.datetime(1980 + off, mo, d, h, mi, s - (s % 2))

    def store_arch_with_dt(path, payload, dt_raw):
        name = b"e.bin"
        crc = zlib.crc32(payload, 0) ^ 0xFFFFFFFF
        main = struct.pack("<BH", 0, 1 << 8) + b"**ACE**"
        main += struct.pack("<BBBB", 20, 20, 12, 0) + struct.pack("<L", dt_raw)
        main += b"\x00" * 8 + b"\x00"
        fb = struct.pack("<BH", 1, 1) + struct.pack("<LL", len(payload), len(payload))
        fb += struct.pack("<LLLBBHHH", dt_raw, 0x20, crc, 0, 0, 0, 0, len(name)) + name
        write_bytes(path, _put(main) + _put(fb) + payload)

    # --- Encoder clamps out-of-range mtimes into the DOS 1980..2107 window ---
    enc_cases = [(1976, 7, 15, 10, 30, 0), (1980, 1, 1, 12, 0, 0),
                 (2100, 3, 4, 5, 6, 8), (2107, 12, 31, 23, 59, 58),
                 (2108, 6, 15, 14, 0, 0)]
    write_bytes(src, data)
    for y, mo, d, h, mi, s in enc_cases:
        ts = datetime.datetime(y, mo, d, h, mi, s).timestamp()
        os.utime(src, (ts, ts))
        mkace("-o", arch, src)
        want = expected_clamp(y, mo, d, h, mi, s)
        check_archive_member(arch, data)
        r = run_ace("d", arch)
        want_str = want.strftime("%Y-%m-%d %H:%M:%S")
        if want_str not in r.stdout:
            raise SystemExit(f"datetime encode {y}: ace d lacks {want_str}\n{r.stdout}")
    os.remove(arch)

    # --- Decoder tolerates raw datetime fields from old/corrupt archives ---
    for dt_raw in (0x00000000, 0xFFFFFFFF, 0x7F000000 | 0x00F80000, (1 << 21) | (1 << 16)):
        store_arch_with_dt(arch, data, dt_raw)
        if run_ace("t", arch).returncode != 0:
            raise SystemExit(f"datetime decode -t failed for raw {dt_raw:#010x}")
        if run_ace("l", arch).returncode != 0:
            raise SystemExit(f"datetime decode -l failed for raw {dt_raw:#010x}")
        if unace_extract_bytes(arch) != data:
            raise SystemExit(f"datetime decode extract mismatch for raw {dt_raw:#010x}")

    os.remove(src)
    os.remove(arch)
    print("ok datetime bounds (encode clamp + decode tolerate)")


def locked_and_file64_check():
    """Verify -k (lock archive) and FILE64 (> 4 GiB representation)."""
    td = os.path.join(ROOT, "testdata")
    src = os.path.join(td, "rt_hello.txt")

    # 1. Test -k (lock archive)
    locked_ace = os.path.join(td, "rt_locked.ace")
    mkace("-k", "-0", "-o", locked_ace, src)
    with open(locked_ace, "rb") as f:
        hdr = f.read(16)
    flags = struct.unpack_from("<H", hdr, 5)[0]
    if not (flags & 0x4000):
        raise SystemExit(f"rt_locked.ace: expected FLAG_LOCKED (0x4000), got {flags:#06x}")
    unace_test(locked_ace)
    os.remove(locked_ace)
    print("ok mkace -k locked (unace)")

    # 2. Test FILE64 (> 4 GiB header format)
    f64_ace = os.path.join(td, "rt_f64.ace")
    env = os.environ.copy()
    env["ACE_FORCE_FILE64"] = "1"
    r = subprocess.run([os.path.join(ROOT, "ace"), "a", f64_ace, "-0", src], cwd=ROOT, env=env, capture_output=True)
    if r.returncode != 0:
        raise SystemExit(f"mkace FILE64 failed:\n{r.stderr.decode()}")
    with open(f64_ace, "rb") as f:
        data = f.read(256)
    main_sz = struct.unpack_from("<H", data, 2)[0]
    fhdr = data[4 + main_sz:]
    htype = fhdr[4]
    hflags = struct.unpack_from("<H", fhdr, 5)[0]
    if htype != 3:
        raise SystemExit(f"rt_f64.ace: expected TYPE_FILE64 (3), got {htype}")
    if not (hflags & 0x0004):
        raise SystemExit("rt_f64.ace: expected FLAG_64BIT")
    unace_test(f64_ace)
    if unace_extract_bytes(f64_ace) != open(src, "rb").read():
        raise SystemExit("rt_f64.ace: data mismatch in unace")
    os.remove(f64_ace)
    print("ok mkace FILE64 (>4GB format) (unace)")


def exclude_and_listfile_check():
    """Verify -x pattern exclusions, -x@list, and @listfile input."""
    td = os.path.join(ROOT, "testdata")
    exdir = os.path.join(td, "_exdir")
    shutil.rmtree(exdir, ignore_errors=True)
    os.makedirs(exdir)
    write_bytes(os.path.join(exdir, "keep.txt"), b"keep me\n")
    write_bytes(os.path.join(exdir, "skip.bak"), b"skip me bak\n")
    write_bytes(os.path.join(exdir, "skip.tmp"), b"skip me tmp\n")

    # Test -x pattern
    ex_ace = os.path.join(td, "rt_ex.ace")
    mkace("-x*.bak", "-x*.tmp", "-o", ex_ace, exdir)
    r = run_ace("l", "-1", ex_ace)
    names = r.stdout.splitlines()
    if not any("keep.txt" in n for n in names) or any(".bak" in n or ".tmp" in n for n in names):
        raise SystemExit(f"mkace -x pattern exclusion failed: {names}")
    os.remove(ex_ace)

    # Test -x@listfile
    ex_list = os.path.join(td, "ex_list.txt")
    write_bytes(ex_list, b"*.bak\n*.tmp\n")
    mkace("-x@" + ex_list, "-o", ex_ace, exdir)
    r = run_ace("l", "-1", ex_ace)
    names = r.stdout.splitlines()
    if not any("keep.txt" in n for n in names) or any(".bak" in n or ".tmp" in n for n in names):
        raise SystemExit(f"mkace -x@list exclusion failed: {names}")
    os.remove(ex_ace)
    os.remove(ex_list)

    # Test @listfile input
    in_list = os.path.join(td, "in_list.txt")
    write_bytes(in_list, f"{os.path.join(exdir, 'keep.txt')}\n{os.path.join(td, 'rt_hello.txt')}\n".encode())
    list_ace = os.path.join(td, "rt_list.ace")
    mkace("-o", list_ace, "@" + in_list)
    r = run_ace("l", "-1", list_ace)
    names = r.stdout.splitlines()
    if not any("keep.txt" in n for n in names) or not any("rt_hello.txt" in n for n in names):
        raise SystemExit(f"mkace @listfile failed: {names}")
    unace_test(list_ace)
    os.remove(list_ace)
    os.remove(in_list)
    shutil.rmtree(exdir, ignore_errors=True)
    print("ok mkace -x exclusions & @listfile (unace)")


def main():
    td = os.path.join(ROOT, "testdata")
    os.makedirs(td, exist_ok=True)

    hello = b"Hello ACE 2.0 blocked member\n"
    write_bytes(os.path.join(td, "rt_hello.txt"), hello)

    sound = bytes((128 + ((i // 4) & 15) - 8) & 0xFF for i in range(256))
    write_bytes(os.path.join(td, "rt_sound.bin"), sound)

    pic = bytes(((i % 64) + (i // 64) * 3) & 0xFF for i in range(64 * 8))
    write_bytes(os.path.join(td, "rt_pic.bin"), pic)

    pic_left = bytes((i * 7) & 0xFF for i in range(64 * 3 + 17))
    write_bytes(os.path.join(td, "rt_pic_left.bin"), pic_left)

    cases = [
        (["-2", "-o", os.path.join(td, "rt_blocked.ace"), os.path.join(td, "rt_hello.txt")],
         os.path.join(td, "rt_blocked.ace"), hello),
        (["-s8", "-o", os.path.join(td, "rt_sound8.ace"), os.path.join(td, "rt_sound.bin")],
         os.path.join(td, "rt_sound8.ace"), sound),
        (["-s16", "-o", os.path.join(td, "rt_sound16.ace"), os.path.join(td, "rt_sound.bin")],
         os.path.join(td, "rt_sound16.ace"), sound),
        (["-s32a", "-o", os.path.join(td, "rt_sound32a.ace"), os.path.join(td, "rt_sound.bin")],
         os.path.join(td, "rt_sound32a.ace"), sound),
        (["-p", "64:1", "-o", os.path.join(td, "rt_pic.ace"), os.path.join(td, "rt_pic.bin")],
         os.path.join(td, "rt_pic.ace"), pic),
        (["-p", "64:1", "-o", os.path.join(td, "rt_pic_left.ace"), os.path.join(td, "rt_pic_left.bin")],
         os.path.join(td, "rt_pic_left.ace"), pic_left),
        (["-z", "-o", os.path.join(td, "rt_lz77.ace"), os.path.join(td, "rt_hello.txt")],
         os.path.join(td, "rt_lz77.ace"), hello),
    ]

    for args, archive, expected in cases:
        mkace(*args)
        unace_test(archive)
        check_archive_member(archive, expected)
        print(f"ok {os.path.basename(archive)}")

    cat = b"the cat sat on the mat the cat sat on the mat\n"
    cat2 = b"the cat sat on the mat again and again and again\n"
    write_bytes(os.path.join(td, "rt_cat1.txt"), cat)
    write_bytes(os.path.join(td, "rt_cat2.txt"), cat2)
    solid = os.path.join(td, "rt_solid.ace")
    mkace("-z", "-s", "-o", solid,
          os.path.join(td, "rt_cat1.txt"), os.path.join(td, "rt_cat2.txt"))
    unace_test(solid)
    check_archive_multi(solid, [cat, cat2])
    print("ok rt_solid.ace")

    # Encrypted + commented member
    pwd = "s3cret"
    main_cm = "MAIN COMMENT"
    file_cm = "FILE COMMENT"
    enc = os.path.join(td, "rt_enc.ace")
    mkace("-z", "-pw", pwd, "-cm", main_cm, "-cf", file_cm,
          "-o", enc, os.path.join(td, "rt_hello.txt"))
    unace_test(enc, pwd=pwd)
    r = run_ace("v", "-p", pwd, enc)
    if main_cm not in r.stdout:
        raise SystemExit(f"rt_enc.ace: main comment missing in verbose listing:\n{r.stdout}")
    if unace_extract_bytes(enc, pwd=pwd) != hello:
        raise SystemExit("unace decrypt/extract mismatch for rt_enc.ace")
    r_bad = subprocess.run([os.path.join(ROOT, "ace"), "t", "-p", "wrong_pwd", enc], capture_output=True)
    if r_bad.returncode == 0:
        raise SystemExit("rt_enc.ace: test with wrong password should fail")
    print("ok rt_enc.ace")

    # Multi-volume fixture test
    r = subprocess.run([sys.executable, os.path.join(ROOT, "tests", "gen_multivol_ace.py")],
                       cwd=ROOT, capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit(f"gen_multivol_ace.py failed\n{r.stderr}")
    mv = os.path.join(td, "mv.ace")
    unace_test(mv)
    mv_expected = b'The quick brown fox jumps over the lazy dog. ' * 3
    if unace_extract_bytes(mv) != mv_expected:
        raise SystemExit("unace multivolume decode mismatch")
    for p in glob.glob(os.path.join(td, "mv.*")):
        os.remove(p)
    print("ok mv.ace")

    cli_extras_checks()
    oem_cross_check()
    multivol_encode_check()
    encoder_modes_check()
    attrs_preserve_check()
    path_preserve_check()
    comment_modes_check()
    dir_recursive_check()
    datetime_bounds_check()
    unified_cli_check()
    locked_and_file64_check()
    exclude_and_listfile_check()
    sfx_support_check()

    for p in glob.glob(os.path.join(td, "rt_*")):
        if os.path.isfile(p):
            os.remove(p)

    print("test_roundtrip: ok")


def sfx_support_check():
    """Verify authentic SFX (Self-Extracting Archive) creation, conversion, and execution."""
    td = os.path.join(ROOT, "testdata")
    src = os.path.join(td, "rt_hello.txt")
    expected = open(src, "rb").read()

    # 1. Direct creation via ace a -sfx
    sfx_exe = os.path.join(td, "rt_sfx.exe")
    r = run_ace("a", "-sfx", sfx_exe, "-z", src)
    if r.returncode != 0 or not os.path.exists(sfx_exe):
        raise SystemExit(f"ace a -sfx failed:\n{r.stdout}{r.stderr}")

    with open(sfx_exe, "rb") as f:
        head = f.read(2)
        if head != b"MZ":
            raise SystemExit(f"rt_sfx.exe missing MZ signature: {head}")
    if os.name == "posix" and not (os.stat(sfx_exe).st_mode & 0o111):
        raise SystemExit("rt_sfx.exe missing executable permissions")

    if run_ace("t", sfx_exe).returncode != 0:
        raise SystemExit("ace t failed on rt_sfx.exe")
    if unace_extract_bytes(sfx_exe) != expected:
        raise SystemExit("unace extract mismatch on rt_sfx.exe")
    os.remove(sfx_exe)

    # 2. Conversion via ace s
    plain_ace = os.path.join(td, "rt_conv.ace")
    mkace("-z", "-o", plain_ace, src)

    r = run_ace("s", plain_ace)
    conv_exe = os.path.join(td, "rt_conv.exe")
    if r.returncode != 0 or not os.path.exists(conv_exe):
        raise SystemExit(f"ace s conversion failed:\n{r.stdout}{r.stderr}")
    if run_ace("t", conv_exe).returncode != 0:
        raise SystemExit("ace t failed on converted conv_exe")
    os.remove(conv_exe)

    # ace s -sfx=win32
    win_exe = os.path.join(td, "rt_win.exe")
    r = run_ace("s", "-sfx=win32", plain_ace, win_exe)
    if r.returncode != 0 or not os.path.exists(win_exe):
        raise SystemExit(f"ace s -sfx=win32 failed:\n{r.stdout}{r.stderr}")
    if run_ace("t", win_exe).returncode != 0:
        raise SystemExit("ace t failed on rt_win.exe")
    os.remove(win_exe)

    # ace s -sfx=gui
    gui_exe = os.path.join(td, "rt_gui.exe")
    r = run_ace("s", "-sfx=gui", plain_ace, gui_exe)
    if r.returncode != 0 or not os.path.exists(gui_exe):
        raise SystemExit(f"ace s -sfx=gui failed:\n{r.stdout}{r.stderr}")
    if run_ace("t", gui_exe).returncode != 0:
        raise SystemExit("ace t failed on rt_gui.exe")
    os.remove(gui_exe)
    os.remove(plain_ace)

    # 3. Autonomous DOSBox self-extraction test
    if shutil.which("dosbox"):
        import tempfile
        tmp_dir = tempfile.mkdtemp(prefix="_sfx_dosbox_", dir=td)
        try:
            greeting = b"Authentic DOS PMODE/W self-extraction verified!\r\n"
            in_file = os.path.join(tmp_dir, "GREET.TXT")
            with open(in_file, "wb") as f:
                f.write(greeting)
            sfx_dos = os.path.join(tmp_dir, "AUTOSFX.EXE")
            r = run_ace("a", "-sfx", sfx_dos, in_file)
            if r.returncode != 0:
                raise SystemExit(f"ace a -sfx failed in dosbox test:\n{r.stderr}")
            os.remove(in_file)

            conf_path = os.path.join(tmp_dir, "dosbox.conf")
            script = [
                "[sdl]",
                "fullscreen=false",
                "output=surface",
                "[cpu]",
                "core=auto",
                "cycles=max",
                "[autoexec]",
                f"mount c {tmp_dir}",
                "c:",
                "AUTOSFX.EXE -y > OUT.TXT",
                "exit",
            ]
            with open(conf_path, "w") as f:
                f.write("\n".join(script) + "\n")
            env = dict(os.environ, SDL_VIDEODRIVER="dummy")
            subprocess.run(["dosbox", "-conf", conf_path], env=env, capture_output=True, timeout=20)

            out_txt = os.path.join(tmp_dir, "OUT.TXT")
            if not os.path.exists(out_txt) or not os.path.exists(in_file):
                raise SystemExit("DOSBox SFX execution failed to produce output or extracted file")
            with open(in_file, "rb") as f:
                extracted = f.read()
            if extracted != greeting:
                raise SystemExit(f"DOSBox SFX extracted payload mismatch: got {extracted} want {greeting}")
            with open(out_txt, "r", errors="ignore") as f:
                log = f.read()
            if "CRC OK" not in log:
                raise SystemExit(f"DOSBox SFX output missing 'CRC OK':\n{log}")
            print("ok authentic SFX DOSBox autonomous execution (CRC OK & payload match)")
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)

    # 4. Linux SFX direct creation & autonomous execution
    linux_sfx = os.path.join(td, "rt_linux.sfx")
    r = run_ace("a", "-sfx=linux", linux_sfx, "-z", src)
    if r.returncode != 0 or not os.path.exists(linux_sfx):
        raise SystemExit(f"ace a -sfx=linux failed:\n{r.stdout}{r.stderr}")
    with open(linux_sfx, "rb") as f:
        head = f.read(4)
        if head != b"\x7fELF":
            raise SystemExit(f"rt_linux.sfx missing ELF signature: {head}")
    if not (os.stat(linux_sfx).st_mode & 0o111):
        raise SystemExit("rt_linux.sfx missing executable permissions")

    # Run list and test directly on the Linux SFX binary
    r = subprocess.run([linux_sfx, "-l"], capture_output=True, text=True)
    if r.returncode != 0 or "hello.txt" not in r.stdout:
        raise SystemExit(f"Linux SFX -l failed:\n{r.stdout}{r.stderr}")

    r = subprocess.run([linux_sfx, "-t"], capture_output=True, text=True)
    if r.returncode != 0 or "OK" not in r.stdout:
        raise SystemExit(f"Linux SFX -t failed:\n{r.stdout}{r.stderr}")

    # Autonomous self-extraction to target directory
    import tempfile
    lin_out = tempfile.mkdtemp(prefix="_sfx_lin_out_", dir=td)
    try:
        r = subprocess.run([linux_sfx, "-d", lin_out], capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"Linux SFX extract failed:\n{r.stdout}{r.stderr}")
        extracted_file = os.path.join(lin_out, "rt_hello.txt")
        if not os.path.exists(extracted_file) or open(extracted_file, "rb").read() != expected:
            raise SystemExit("Linux SFX extracted content mismatch")
    finally:
        shutil.rmtree(lin_out, ignore_errors=True)

    # Verification via ace cli
    if run_ace("t", linux_sfx).returncode != 0:
        raise SystemExit("ace t failed on rt_linux.sfx")
    if unace_extract_bytes(linux_sfx) != expected:
        raise SystemExit("unace extract mismatch on rt_linux.sfx")
    os.remove(linux_sfx)

    # 5. Conversion to Linux SFX via ace s -sfx=linux
    plain_ace2 = os.path.join(td, "rt_conv2.ace")
    mkace("-z", "-o", plain_ace2, src)
    r = run_ace("s", "-sfx=linux", plain_ace2)
    conv_sfx = os.path.join(td, "rt_conv2.sfx")
    if r.returncode != 0 or not os.path.exists(conv_sfx):
        raise SystemExit(f"ace s -sfx=linux conversion failed:\n{r.stdout}{r.stderr}")
    r = subprocess.run([conv_sfx, "-t"], capture_output=True, text=True)
    if r.returncode != 0:
        raise SystemExit("Linux converted SFX -t failed")
    os.remove(conv_sfx)
    os.remove(plain_ace2)

    print("ok authentic SFX creation & conversion (dos/win32/gui/linux)")

    test_path_traversal_security()


def test_path_traversal_security():
    """Verify that both ace x and native Linux SFX reject path traversal attacks."""
    import tempfile
    import time

    td = os.path.join(ROOT, "testdata")
    os.makedirs(td, exist_ok=True)
    bad_archive = os.path.join(td, "traversal_attack.ace")

    def ace_crc32(buf: bytes) -> int:
        return zlib.crc32(buf, 0) ^ 0xFFFFFFFF

    def ace_crc16(buf: bytes) -> int:
        return ace_crc32(buf) & 0xFFFF

    def dos_time(ts=None) -> int:
        t = time.localtime(ts)
        return ((t.tm_year - 1980) << 25) | (t.tm_mon << 21) | (t.tm_mday << 16) | (
            t.tm_hour << 11) | (t.tm_min << 5) | (t.tm_sec // 2)

    def put_header(payload: bytes) -> bytes:
        return struct.pack('<HH', ace_crc16(payload), len(payload)) + payload

    dt = dos_time()
    main_body = struct.pack('<BH', 0, 1 << 8)  # type 0, V20FORMAT
    main_body += b'**ACE**'
    main_body += struct.pack('<BBBB', 20, 20, 12, 0)  # e/c 2.0, Linux, vol 0
    main_body += struct.pack('<L', dt)
    main_body += b'\x00' * 8
    main_body += b'\x00'

    blob = put_header(main_body)

    files = [
        (b"../escape_posix.txt", b"evil posix traversal"),
        (b"..\\escape_dos.txt", b"evil dos traversal"),
        (b"/tmp/root_posix.txt", b"evil absolute posix"),
        (b"\\tmp\\root_dos.txt", b"evil absolute dos"),
        (b"C:\\autoexec.bat", b"evil drive C:"),
        (b"D:relative_drive.txt", b"evil drive D:"),
        (b"DIR\\SUBDIR\\SAFE.TXT", b"safe dos normalized"),
    ]

    for fname, data in files:
        fflags = 1  # ADDSIZE
        file_body = struct.pack('<BH', 1, fflags)
        file_body += struct.pack('<LL', len(data), len(data))
        file_body += struct.pack('<LLLBBHHH',
                                 dt,
                                 0x20,
                                 ace_crc32(data),
                                 0,  # stored
                                 0,
                                 0,
                                 0,
                                 len(fname))
        file_body += fname
        blob += put_header(file_body) + data

    with open(bad_archive, "wb") as f:
        f.write(blob)

    # 1. Test extraction with `ace x`
    dest_dir = tempfile.mkdtemp(prefix="_test_trav_ace_", dir=td)
    try:
        r = subprocess.run([os.path.join(ROOT, "ace"), "x", "-d", dest_dir, bad_archive],
                           capture_output=True, text=True)
        assert "Security warning: skipping unsafe path traversal" in r.stderr, (
            f"Expected security warning in stderr, got:\n{r.stderr}"
        )
        assert not os.path.exists(os.path.join(td, "escape_posix.txt"))
        assert not os.path.exists(os.path.join(td, "escape_dos.txt"))
        assert not os.path.exists("/tmp/root_posix.txt")
        assert not os.path.exists(os.path.join(dest_dir, "escape_posix.txt"))
        assert not os.path.exists(os.path.join(dest_dir, "C:\\autoexec.bat"))

        # Verify safe historical DOS path DIR\SUBDIR\SAFE.TXT was normalized to DIR/SUBDIR/SAFE.TXT
        safe_path = os.path.join(dest_dir, "DIR", "SUBDIR", "SAFE.TXT")
        assert os.path.exists(safe_path), f"Expected safe file at {safe_path}"
        assert open(safe_path, "rb").read() == b"safe dos normalized"
    finally:
        shutil.rmtree(dest_dir, ignore_errors=True)

    # 2. Test extraction with native Linux SFX
    sfx_bad = os.path.join(td, "traversal_attack.sfx")
    r = subprocess.run([os.path.join(ROOT, "ace"), "s", "-sfx=linux", bad_archive, sfx_bad],
                       capture_output=True, text=True)
    assert r.returncode == 0, f"ace s -sfx=linux failed:\n{r.stderr}"

    sfx_dest = tempfile.mkdtemp(prefix="_test_trav_sfx_", dir=td)
    try:
        r = subprocess.run([sfx_bad, "-d", sfx_dest], capture_output=True, text=True)
        assert "Security warning: skipping unsafe path traversal" in r.stderr, (
            f"Expected security warning in sfx stderr, got:\n{r.stderr}"
        )
        assert not os.path.exists(os.path.join(td, "escape_posix.txt"))
        assert not os.path.exists(os.path.join(td, "escape_dos.txt"))
        safe_sfx_path = os.path.join(sfx_dest, "DIR", "SUBDIR", "SAFE.TXT")
        assert os.path.exists(safe_sfx_path), f"Expected safe file at {safe_sfx_path}"
        assert open(safe_sfx_path, "rb").read() == b"safe dos normalized"
    finally:
        shutil.rmtree(sfx_dest, ignore_errors=True)
        if os.path.exists(sfx_bad):
            os.remove(sfx_bad)
        if os.path.exists(bad_archive):
            os.remove(bad_archive)

    print("ok path traversal & DOS backslash normalization security checks")

    test_bare_and_unix_listing()


def test_bare_and_unix_listing():
    """Verify -1 (--bare) and -u (--unix) options for ace l and Linux SFX."""
    import tempfile

    td = os.path.join(ROOT, "testdata")
    src_dir = tempfile.mkdtemp(prefix="_test_bare_src_", dir=td)
    dest_dir = tempfile.mkdtemp(prefix="_test_bare_dst_", dir=td)
    arc_path = os.path.join(td, "test_bare.ace")
    sfx_path = os.path.join(td, "test_bare.sfx")

    try:
        # Create test files with uppercase names and subdirectories
        sub = os.path.join(src_dir, "SUB")
        os.makedirs(sub, exist_ok=True)
        with open(os.path.join(src_dir, "HELLO.TXT"), "wb") as f:
            f.write(b"hello world\n")
        with open(os.path.join(sub, "DATA.BIN"), "wb") as f:
            f.write(b"binary data\n")

        # Create archive with relative paths (-A)
        r = subprocess.run([os.path.join(ROOT, "ace"), "a", "-A", arc_path, "HELLO.TXT", "SUB/DATA.BIN"],
                           cwd=src_dir, capture_output=True, text=True)
        assert r.returncode == 0, f"ace a failed:\n{r.stderr}"

        # 1. Test ace l -1 (bare output)
        r = subprocess.run([os.path.join(ROOT, "ace"), "l", "-1", arc_path],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"ace l -1 failed:\n{r.stderr}"
        lines = [line.strip() for line in r.stdout.strip().splitlines() if line.strip()]
        assert "HELLO.TXT" in lines
        assert "SUB/DATA.BIN" in lines
        assert len(lines) == 2, f"Expected exactly 2 lines in bare output, got {lines}"

        # 2. Test ace l --bare
        r = subprocess.run([os.path.join(ROOT, "ace"), "l", "--bare", arc_path],
                           capture_output=True, text=True)
        assert r.returncode == 0
        lines = [line.strip() for line in r.stdout.strip().splitlines() if line.strip()]
        assert len(lines) == 2

        # 3. Test ace l -u (Unix formatted paths: lowercase)
        r = subprocess.run([os.path.join(ROOT, "ace"), "l", "-u", arc_path],
                           capture_output=True, text=True)
        assert r.returncode == 0
        assert "hello.txt" in r.stdout
        assert "sub/data.bin" in r.stdout

        # 4. Test ace l -1 -u (bare + unix combined)
        r = subprocess.run([os.path.join(ROOT, "ace"), "l", "-1", "-u", arc_path],
                           capture_output=True, text=True)
        assert r.returncode == 0
        lines = [line.strip() for line in r.stdout.strip().splitlines() if line.strip()]
        assert lines == ["hello.txt", "sub/data.bin"]

        # 5. Test ace x -u (extract with lowercase filenames)
        r = subprocess.run([os.path.join(ROOT, "ace"), "x", "-u", "-d", dest_dir, arc_path],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"ace x -u failed:\n{r.stderr}"
        assert os.path.exists(os.path.join(dest_dir, "hello.txt")), "Expected lowercase hello.txt on disk"
        assert os.path.exists(os.path.join(dest_dir, "sub", "data.bin")), "Expected lowercase sub/data.bin on disk"

        # 6. Test Linux standalone SFX stub with -1 and -u
        r = subprocess.run([os.path.join(ROOT, "ace"), "s", "-sfx=linux", arc_path, sfx_path],
                           capture_output=True, text=True)
        assert r.returncode == 0, f"ace s -sfx=linux failed:\n{r.stderr}"

        # SFX -l -1
        r = subprocess.run([sfx_path, "-l", "-1"], capture_output=True, text=True)
        assert r.returncode == 0
        lines = [line.strip() for line in r.stdout.strip().splitlines() if line.strip()]
        assert len(lines) == 2
        assert "HELLO.TXT" in lines

        # SFX -l -1 -u
        r = subprocess.run([sfx_path, "-l", "-1", "-u"], capture_output=True, text=True)
        assert r.returncode == 0
        lines = [line.strip() for line in r.stdout.strip().splitlines() if line.strip()]
        assert lines == ["hello.txt", "sub/data.bin"]

    finally:
        shutil.rmtree(src_dir, ignore_errors=True)
        shutil.rmtree(dest_dir, ignore_errors=True)
        if os.path.exists(arc_path):
            os.remove(arc_path)
        if os.path.exists(sfx_path):
            os.remove(sfx_path)

    print("ok -1 (--bare) and -u (--unix) listing & extraction")


if __name__ == "__main__":
    main()

