#!/usr/bin/env python3
"""Create a minimal but valid arm64 Linux Image header (for packing smoke tests)."""
import struct, sys

out = sys.argv[1] if len(sys.argv) > 1 else 'out/fake-image'
body = b'\x00' * 0x2000
img = bytearray(64 + len(body))
struct.pack_into('<IIQQQQQQI', img, 0,
                 0x14000000,            # code0: b .
                 0,                     # code1
                 0,                     # text_offset
                 len(img),              # image_size (covers .bss)
                 0xa,                   # flags: LE, 4K pages, place anywhere
                 0, 0, 0,               # res2..4
                 0x644d5241)            # "ARM\x64"
img[64:] = body
open(out, 'wb').write(bytes(img))
print(out, len(img), 'bytes')
