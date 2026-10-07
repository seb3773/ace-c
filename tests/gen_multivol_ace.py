#!/usr/bin/env python3
"""Generate a two-volume ACE 2.0 stored archive for tests.

The single member is split across the two volumes using the CONTNEXT /
CONTPREV file flags.  The first volume is written to <out>.ace and the
second to <out>.c00.
"""
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


V20FORMAT = 1 << 8
MULTIVOLUME = 1 << 11
ADDSIZE = 1 << 0
CONTNEXT = 1 << 13
CONTPREV = 1 << 12


def main_header(volume: int) -> bytes:
    flags = V20FORMAT | MULTIVOLUME
    body = struct.pack('<BH', 0, flags)
    body += b'**ACE**'
    body += struct.pack('<BBBB', 20, 20, 12, volume)
    body += struct.pack('<L', dos_time())
    body += b'\x00' * 8
    body += b'\x00'
    return put_header(body)


def file_header(name: bytes, packsize: int, origsize: int, crc: int, flags: int) -> bytes:
    body = struct.pack('<BH', 1, flags)
    body += struct.pack('<LL', packsize, origsize)
    body += struct.pack('<LLLBBHHH',
                        dos_time(),
                        0x20,
                        crc,
                        0,
                        0,
                        6,
                        0,
                        len(name))
    body += name
    return put_header(body)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else 'testdata/mv'
    data = b'The quick brown fox jumps over the lazy dog. ' * 3
    name = b'payload.txt'
    crc = ace_crc32(data)
    split = 40
    part1, part2 = data[:split], data[split:]

    vol1 = main_header(0)
    vol1 += file_header(name, len(part1), len(data), crc, ADDSIZE | CONTNEXT)
    vol1 += part1

    vol2 = main_header(1)
    vol2 += file_header(name, len(part2), len(data), crc, ADDSIZE | CONTPREV)
    vol2 += part2

    os.makedirs(os.path.dirname(out) or '.', exist_ok=True)
    with open(out + '.ace', 'wb') as f:
        f.write(vol1)
    with open(out + '.c00', 'wb') as f:
        f.write(vol2)
    print(f'wrote {out}.ace ({len(vol1)} bytes) and {out}.c00 ({len(vol2)} bytes)')


if __name__ == '__main__':
    main()
