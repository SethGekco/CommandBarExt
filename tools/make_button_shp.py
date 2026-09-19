#!/usr/bin/env python3
"""Generate a placeholder TS-format SHP for a new command-bar button.

The engine loads bar button art as Button%02d.SHP; button SHPs carry
Up / Down / Disabled frames (ShapeButtonFlag in YRpp). This emits an
uncompressed 3-frame SHP with a simple bordered square so the probe
button is visible and clickable. Replace with real art later.

Usage: make_button_shp.py <out.shp> [width height]
"""
import struct
import sys


def frame_pixels(w, h, fill, border):
    px = bytearray(w * h)
    for y in range(h):
        for x in range(w):
            edge = x in (0, w - 1) or y in (0, h - 1)
            px[y * w + x] = border if edge else fill
    return bytes(px)


def main():
    out = sys.argv[1]
    w = int(sys.argv[2]) if len(sys.argv) > 3 else 33
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 24
    # Up / Down / Disabled: same square, different fill indices so the
    # states are distinguishable in-game even as a placeholder.
    frames = [frame_pixels(w, h, fill, 15) for fill in (128, 130, 12)]

    header = struct.pack('<4H', 0, w, h, len(frames))
    fh_size = 24
    data_off = len(header) + fh_size * len(frames)

    fhdrs = []
    body = b''
    for px in frames:
        # x, y, cx, cy, flags(0 = uncompressed), color, reserved, offset
        fhdrs.append(struct.pack('<4H4I', 0, 0, w, h, 0, 0, 0, data_off + len(body)))
        body += px

    with open(out, 'wb') as f:
        f.write(header + b''.join(fhdrs) + body)
    print(f'{out}: {w}x{h}, {len(frames)} frames, {len(header) + fh_size * len(frames) + len(body)} bytes')


if __name__ == '__main__':
    main()
