#!/usr/bin/env python3
"""Whether two pictures are the same picture, pixel for pixel.

usage: same-picture.py <a> <b>

Each is a TGA as chimera-run --screenshot writes it (32 bits a pixel) or a
PPM as the gate's runners write it (--ppm). Alpha is not compared: a PPM has
none. Exit 0 and "same WxH" when they are; 1 and how many pixels differ when
they are not.
"""
import struct
import sys


def load(path):
    d = open(path, "rb").read()
    if d[:2] == b"P6":
        _, size, _, pixels = d.split(b"\n", 3)
        w, h = map(int, size.split())
        return w, h, pixels[: w * h * 3]
    id_length = d[0]
    w, h, bpp, descriptor = struct.unpack("<HHBB", d[12:18])
    if bpp != 32:
        sys.exit(f"{path}: {bpp} bits a pixel, and 32 were expected")
    pixels = d[18 + id_length : 18 + id_length + w * h * 4]
    rows = [pixels[y * w * 4 : (y + 1) * w * 4] for y in range(h)]
    if not descriptor & 0x20:  # bottom row first
        rows.reverse()
    rgb = bytearray(w * h * 3)
    joined = b"".join(rows)
    rgb[0::3] = joined[2::4]
    rgb[1::3] = joined[1::4]
    rgb[2::3] = joined[0::4]
    return w, h, bytes(rgb)


def main():
    aw, ah, a = load(sys.argv[1])
    bw, bh, b = load(sys.argv[2])
    if (aw, ah) != (bw, bh):
        print(f"{aw}x{ah} against {bw}x{bh}")
        return 1
    if a == b:
        if len(set(a[i : i + 3] for i in range(0, len(a), 3 * 257))) < 3:
            print(f"same {aw}x{ah}, and blank")
            return 1
        print(f"same {aw}x{ah}")
        return 0
    differ = sum(1 for i in range(0, len(a), 3) if a[i : i + 3] != b[i : i + 3])
    print(f"{differ} of {aw * ah} pixels differ")
    return 1


if __name__ == "__main__":
    sys.exit(main())
