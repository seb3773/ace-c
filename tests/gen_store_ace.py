#!/usr/bin/env python3
"""Generate a minimal ACE 2.0 stored (uncompressed) archive for tests."""
import os
import struct
import sys
import time
import zlib


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


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'testdata/hello.ace'
    src = sys.argv[2] if len(sys.argv) > 2 else None
    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    if src:
        with open(src, 'rb') as f:
            data = f.read()
        name = os.path.basename(src).encode('utf-8')
    else:
        data = b'Hello ACE 2.6 stored member\n'
        name = b'hello.txt'
        os.makedirs('testdata', exist_ok=True)
        with open('testdata/hello.txt', 'wb') as f:
            f.write(data)

    # MAIN header: type, flags, magic, eversion, cversion, host, volume, datetime, reserved1, advert_len
    flags = 1 << 8  # V20FORMAT
    dt = dos_time()
    main_body = struct.pack('<BH', 0, flags)
    main_body += b'**ACE**'
    main_body += struct.pack('<BBBB', 20, 20, 12, 0)  # e/c version 2.0, Linux, vol 0
    main_body += struct.pack('<L', dt)
    main_body += b'\x00' * 8
    main_body += b'\x00'  # advert size 0

    # FILE32 header
    fflags = 1  # ADDSIZE
    file_body = struct.pack('<BH', 1, fflags)
    file_body += struct.pack('<LL', len(data), len(data))
    file_body += struct.pack('<LLLBBHHH',
                             dt,
                             0x20,  # ARCHIVE
                             ace_crc32(data),
                             0,  # stored
                             0,  # none
                             0,  # params: 1K dict bits (0+10)
                             0,
                             len(name))
    file_body += name

    blob = put_header(main_body) + put_header(file_body) + data
    with open(out, 'wb') as f:
        f.write(blob)
    print(f'wrote {out} ({len(blob)} bytes, {len(data)} payload)')


if __name__ == '__main__':
    main()
