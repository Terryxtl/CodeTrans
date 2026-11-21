"""Generates codetrans.ico (16..64 px, 32-bit BMP entries) without any deps.

    python -I windows/res/make_icon.py windows/res/codetrans.ico

Design: rounded blue tile with a white 7x7 "QR" glyph (three finder
patterns + a few data modules).
"""
import struct
import sys

GLYPH = [
    "XXX.XXX",
    "X.X.X.X",
    "XXX.XXX",
    "...X.X.",
    "XXX.XX.",
    "X.X..XX",
    "XXX.X.X",
]
BG = (0xD6, 0x78, 0x00, 0xFF)      # BGRA: #0078D6
FG = (0xFF, 0xFF, 0xFF, 0xFF)


def render(size):
    px = [[(0, 0, 0, 0)] * size for _ in range(size)]
    r = max(2, size // 6)            # corner radius
    for y in range(size):
        for x in range(size):
            cx = min(x, size - 1 - x)
            cy = min(y, size - 1 - y)
            if cx < r and cy < r and (r - cx - 0.5) ** 2 + (r - cy - 0.5) ** 2 > r * r:
                continue
            px[y][x] = BG
    n = len(GLYPH)
    cell = max(1, (size * 3 // 4) // n)
    off = (size - cell * n) // 2
    for gy, row in enumerate(GLYPH):
        for gx, c in enumerate(row):
            if c != "X":
                continue
            for dy in range(cell):
                for dx in range(cell):
                    px[off + gy * cell + dy][off + gx * cell + dx] = FG
    return px


def bmp_entry(size):
    px = render(size)
    hdr = struct.pack("<IiiHHIIiiII", 40, size, size * 2, 1, 32, 0, 0, 0, 0, 0, 0)
    xor = b"".join(bytes(c for p in px[y] for c in p) for y in range(size - 1, -1, -1))
    row = ((size + 31) // 32) * 4
    and_mask = b"\x00" * (row * size)     # alpha channel is authoritative
    return hdr + xor + and_mask


def main(out):
    sizes = [16, 20, 24, 32, 40, 48, 64]
    blobs = [bmp_entry(s) for s in sizes]
    head = struct.pack("<HHH", 0, 1, len(sizes))
    offset = 6 + 16 * len(sizes)
    dirs = b""
    for s, b in zip(sizes, blobs):
        dirs += struct.pack("<BBBBHHII", s % 256, s % 256, 0, 0, 1, 32, len(b), offset)
        offset += len(b)
    with open(out, "wb") as f:
        f.write(head + dirs + b"".join(blobs))


if __name__ == "__main__":
    main(sys.argv[1] if len(sys.argv) > 1 else "codetrans.ico")
