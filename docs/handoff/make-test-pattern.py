#!/usr/bin/env python3
"""Generate the 1080x2460 BGRA test pattern used for display debugging.

Quadrants: red (top-left), green (top-right), blue (bottom-left), white
(bottom-right), plus a 2px white border and a 2px white line down the middle.
Write it straight into the framebuffer:  dd if=pattern.rgb of=/dev/fb0 bs=4096
"""
W, H = 1080, 2460
buf = bytearray(W * H * 4)
for y in range(H):
    row = y * W * 4
    top, left = y < H // 2, None
    for x in range(W):
        o = row + x * 4
        if y < H // 2:
            buf[o:o+3] = bytes((0, 0, 255)) if x < W // 2 else bytes((0, 255, 0))
        else:
            buf[o:o+3] = bytes((255, 0, 0)) if x < W // 2 else bytes((255, 255, 255))
        buf[o + 3] = 0xff
for x in range(W):
    for y in (0, 1, H - 2, H - 1):
        o = (y * W + x) * 4
        buf[o:o+4] = bytes((255, 255, 255, 255))
for y in range(H):
    for x in (0, 1, W // 2 - 1, W // 2, W - 2, W - 1):
        o = (y * W + x) * 4
        buf[o:o+4] = bytes((255, 255, 255, 255))
open('pattern.rgb', 'wb').write(buf)
print('pattern.rgb', len(buf), 'bytes')
