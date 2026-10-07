#!/usr/bin/env python3
"""Test native ace against authentic WinACE 2.6 corpus archives."""
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CORPUS = os.path.join(ROOT, "testdata", "winace")
ACE = os.path.join(ROOT, "ace")


def test_recompression():
    """Verify that re-compressing corpus files matches exact WinACE 2.6 targets."""
    # 1. Verify pic.ace target: exactly 183 bytes, bit-exact 88-byte payload
    pic_ref = os.path.join(CORPUS, "pic.ace")
    tmp_extract = os.path.join(ROOT, "testdata", "_tmp_extract_pic")
    os.makedirs(tmp_extract, exist_ok=True)
    subprocess.run([ACE, "x", "-d", tmp_extract, pic_ref], cwd=ROOT, check=True, capture_output=True)
    pic_bin = os.path.join(ROOT, "PIC.BIN")
    shutil.copyfile(os.path.join(tmp_extract, "PIC.BIN"), pic_bin)
    shutil.rmtree(tmp_extract, ignore_errors=True)

    out_pic = os.path.join(ROOT, "testdata", "_rt_pic.ace")
    try:
        subprocess.run([ACE, "a", out_pic, "-2", "PIC.BIN"], cwd=ROOT, check=True, capture_output=True)
        sz = os.path.getsize(out_pic)
        if sz != 183:
            raise SystemExit(f"pic.ace recompression size mismatch: got {sz}, expected 183")
        with open(out_pic, "rb") as fp1, open(pic_ref, "rb") as fp2:
            pay1 = fp1.read()[95:]
            pay2 = fp2.read()[95:]
            if pay1 != pay2:
                raise SystemExit("pic.ace payload is not bit-exact to WinACE reference")
        print("ok pic.ace recompression (183 bytes, bit-exact payload)")
    finally:
        if os.path.exists(pic_bin):
            os.remove(pic_bin)
        if os.path.exists(out_pic):
            os.remove(out_pic)

    # 2. Verify best.ace target: exactly 596 bytes
    best_ref = os.path.join(CORPUS, "best.ace")
    tmp_extract_best = os.path.join(ROOT, "testdata", "_tmp_extract_best")
    os.makedirs(tmp_extract_best, exist_ok=True)
    subprocess.run([ACE, "x", "-d", tmp_extract_best, best_ref], cwd=ROOT, check=True, capture_output=True)
    member_names = sorted(os.listdir(tmp_extract_best))
    created_files = []
    out_best = os.path.join(ROOT, "testdata", "_rt_best.ace")
    try:
        for fname in member_names:
            src = os.path.join(tmp_extract_best, fname)
            dst = os.path.join(ROOT, fname)
            shutil.copyfile(src, dst)
            created_files.append(dst)
        cmd = [ACE, "a", out_best, "-2", "-m", "5"] + member_names
        subprocess.run(cmd, cwd=ROOT, check=True, capture_output=True)
        sz = os.path.getsize(out_best)
        if sz != 596:
            raise SystemExit(f"best.ace recompression size mismatch: got {sz}, expected 596")
        print("ok best.ace recompression (596 bytes, exact size match)")
    finally:
        shutil.rmtree(tmp_extract_best, ignore_errors=True)
        for p in created_files:
            if os.path.exists(p):
                os.remove(p)
        if os.path.exists(out_best):
            os.remove(out_best)


def main():
    if not os.path.isdir(CORPUS):
        print("skip: testdata/winace missing")
        return
    archives = sorted(
        os.path.join(CORPUS, n) for n in os.listdir(CORPUS) if n.endswith(".ace")
    )
    if not archives:
        print("skip: no winace archives")
        return
    for path in archives:
        r = subprocess.run([ACE, "t", path], cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0:
            raise SystemExit(f"ace t failed: {path}\n{r.stdout}{r.stderr}")
        tmp_dir = os.path.join(ROOT, "testdata", "_tmp_test_extract")
        os.makedirs(tmp_dir, exist_ok=True)
        try:
            rx = subprocess.run([ACE, "x", "-d", tmp_dir, path], cwd=ROOT, capture_output=True, text=True)
            if rx.returncode != 0:
                raise SystemExit(f"ace x failed: {path}\n{rx.stdout}{rx.stderr}")
        finally:
            shutil.rmtree(tmp_dir, ignore_errors=True)
        print(f"ok {os.path.basename(path)}")
    test_recompression()
    print("winace_corpus: ok")


if __name__ == "__main__":
    main()
