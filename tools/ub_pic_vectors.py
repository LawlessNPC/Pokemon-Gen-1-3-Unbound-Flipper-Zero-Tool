#!/usr/bin/env python3
"""Writes tests/ub_pic_vectors.h: test vectors for src/gen3/gen3_ub_pic.c.

Usage: python3 -I tools/ub_pic_vectors.py

Standard library only. Each vector is a 64x64 front picture as the GBA stores it (4bpp tiles
and a BGR555 palette), the 404-byte record tools/gen3_sprites.py makes of it (convert and
fxbm_record, imported unchanged), and the tiles compressed in the GBA BIOS LZ77 format by a
simple greedy encoder. The output is deterministic.

The C code compares luminance in integers (299 r + 587 g + 114 b, against the thresholds
times 1000), the script's convert in floats. So that the two cannot disagree, every vector is
checked here: for the colours it uses, the float and the integer decisions are computed both
ways, must agree, and no colour may sit exactly on a threshold or exactly one line margin from
another.
"""

import os
import sys

sys.dont_write_bytecode = True
TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, TOOLS_DIR)
import gen3_sprites as g  # noqa: E402

OUTPUT = os.path.join(os.path.dirname(TOOLS_DIR), "tests", "ub_pic_vectors.h")

LUM_BLACK_K = g.LUM_BLACK * 1000
LUM_WHITE_K = g.LUM_WHITE * 1000
MARGIN_K = g.LINE_MARGIN * 1000
TRANSPARENT_SHADE_K = 255000


def rgb8(c5):
    return tuple(v * 8 for v in c5)


def bgr555(c5):
    r, g_, b = c5
    return r | g_ << 5 | b << 10


def lum_k(c5):
    r, g_, b = rgb8(c5)
    return 299 * r + 587 * g_ + 114 * b


def check_palette(palette5, pixels, name):
    """Float and integer decisions must agree, with no exact tie."""
    used = sorted({i for row in pixels for i in row})
    palette8 = [rgb8(c) for c in palette5]
    flo = [0.299 * r + 0.587 * g_ + 0.114 * b for r, g_, b in palette8]
    exact = [lum_k(c) for c in palette5]
    shade_f = {i: (flo[i] if i else 255.0) for i in used}
    shade_k = {i: (exact[i] if i else TRANSPARENT_SHADE_K) for i in used}
    for i in used:
        if i == 0:
            continue
        for t_k, t_f in ((LUM_BLACK_K, g.LUM_BLACK), (LUM_WHITE_K, g.LUM_WHITE)):
            assert exact[i] != t_k, f"{name}: colour {i} sits exactly on {t_f}"
            assert (flo[i] < t_f) == (exact[i] < t_k), f"{name}: colour {i} float and integer differ"
    for a in used:
        for b in used:
            assert shade_k[b] - shade_k[a] != MARGIN_K, f"{name}: {a},{b} exactly one margin apart"
            assert (shade_f[b] > shade_f[a] + g.LINE_MARGIN) == (
                shade_k[b] > shade_k[a] + MARGIN_K
            ), f"{name}: {a},{b} float and integer differ"


# ---------------------------------------------------------------------------
# Pictures


def blank():
    return [[0] * 64 for _ in range(64)]


def lcg(seed):
    state = seed
    while True:
        state = (state * 1103515245 + 12345) & 0x7FFFFFFF
        yield state >> 8


def ellipse(px, cx, cy, rx, ry, colour):
    for y in range(64):
        for x in range(64):
            if ((x - cx) / rx) ** 2 + ((y - cy) / ry) ** 2 <= 1.0:
                px[y][x] = colour


def rect(px, x0, y0, x1, y1, colour):
    for y in range(max(y0, 0), min(y1, 63) + 1):
        for x in range(max(x0, 0), min(x1, 63) + 1):
            px[y][x] = colour


# Greys: lum = 8 c exactly, so none is on a threshold (60, 165) or a margin (35) from another
GREYS = {
    "black": (2, 2, 2),
    "dark": (9, 9, 9),
    "mid": (15, 15, 15),
    "light": (23, 23, 23),
    "white": (31, 31, 31),
}


def palette_of(*colours):
    palette = [(0, 0, 0)] * 16
    for i, c in enumerate(colours, start=1):
        palette[i] = c
    return palette


def v_small():
    px = blank()
    ellipse(px, 31, 35, 11, 14, 1)
    ellipse(px, 31, 38, 7, 8, 2)
    rect(px, 27, 28, 28, 29, 3)
    rect(px, 34, 28, 35, 29, 3)
    rect(px, 29, 40, 33, 40, 3)
    ellipse(px, 31, 22, 4, 3, 4)
    pal = palette_of(GREYS["white"], GREYS["mid"], GREYS["black"], GREYS["dark"])
    return "small figure", px, pal


def v_full():
    # 64 wide and 64 tall, in runs of alike columns and rows, so that columns and rows get
    # dropped and the first-minimum rule decides among equals
    rnd = lcg(7)
    px = blank()
    for y in range(64):
        for x in range(64):
            if (x - 31.5) ** 2 + (y - 31.5) ** 2 > 33.5**2:
                continue
            px[y][x] = 1 + ((x // 5) + (y // 3)) % 4
    for _ in range(40):
        x, y = next(rnd) % 64, next(rnd) % 64
        if px[y][x]:
            px[y][x] = 1 + next(rnd) % 4
    pal = palette_of(GREYS["white"], GREYS["mid"], GREYS["black"], GREYS["light"])
    return "full 64x64, columns and rows dropped", px, pal


def v_rows():
    px = blank()
    ellipse(px, 31.5, 31.5, 25.5, 32.5, 1)
    for y in range(8, 60, 7):
        rect(px, 10, y, 52, y + 1, 2)
    ellipse(px, 31.5, 20, 6, 5, 3)
    pal = palette_of(GREYS["light"], GREYS["mid"], GREYS["black"])
    return "64 tall, 52 wide, rows dropped only", px, pal


def v_cols():
    px = blank()
    ellipse(px, 31.5, 33, 31.5, 22, 1)
    for x in range(6, 60, 6):
        rect(px, x, 14, x, 52, 2)
    pal = palette_of(GREYS["white"], GREYS["mid"])
    return "64 wide, 44 tall, columns dropped only", px, pal


def v_lines():
    px = blank()
    ellipse(px, 31.5, 31.5, 22, 26, 1)
    rect(px, 14, 20, 49, 20, 4)  # a horizontal line
    rect(px, 24, 12, 24, 50, 4)  # a vertical line
    for k in range(20):  # a diagonal line
        px[18 + k][34 + k // 2 + k % 2 * 0] = 4
    for k in range(16):  # a diagonal line the other way
        px[40 - k][14 + k] = 4
    rect(px, 38, 26, 46, 27, 4)  # two pixels wide: not a line
    rect(px, 40, 40, 44, 44, 2)  # a patch of darker grey
    px[42][42] = 5  # a pixel only slightly darker than its patch: not a line
    rect(px, 30, 32, 31, 32, 3)
    pal = palette_of(
        GREYS["white"], GREYS["mid"], GREYS["black"], GREYS["dark"], (13, 13, 13)
    )
    return "one-pixel lines", px, pal


def v_edges_cut():
    px = blank()
    ellipse(px, 0, 0, 40, 36, 1)
    ellipse(px, 63, 63, 38, 34, 1)
    ellipse(px, 8, 8, 9, 9, 2)
    ellipse(px, 55, 55, 9, 9, 2)
    px[0][0] = 3
    px[63][63] = 3
    px[0][63] = 1
    px[63][0] = 1
    pal = palette_of(GREYS["white"], GREYS["mid"], GREYS["black"])
    return "touching the picture's edges", px, pal


def v_edges_frame():
    # exactly 56x56, a diamond with a border: touches every edge of the frame
    px = blank()
    for y in range(56):
        for x in range(56):
            px[y + 4][x + 2] = 1 + ((x + y) // 4) % 3 if abs(x - 27.5) + abs(y - 27.5) <= 40 else 0
    px[4][29] = 1
    px[59][30] = 1
    px[31][2] = 1
    px[32][57] = 1
    pal = palette_of(GREYS["white"], GREYS["mid"], GREYS["light"])
    return "exactly 56x56, touching the frame", px, pal


def search_lum(target_k, below):
    """The 5-bit colour whose luminance is nearest the target on one side, never on it."""
    best = None
    for r in range(32):
        for g_ in range(32):
            for b in range(32):
                lk = lum_k((r, g_, b))
                if (lk < target_k) if below else (lk > target_k):
                    d = abs(lk - target_k)
                    if best is None or d < best[0]:
                        best = (d, (r, g_, b))
    return best[1]


def v_thresholds():
    colours = [
        search_lum(LUM_BLACK_K, True),
        search_lum(LUM_BLACK_K, False),
        search_lum(LUM_WHITE_K, True),
        search_lum(LUM_WHITE_K, False),
    ]
    px = blank()
    ellipse(px, 31.5, 31.5, 26, 27, 5)
    for k in range(4):
        rect(px, 12 + k * 10, 14, 19 + k * 10, 50, k + 1)
    for k in range(4):
        rect(px, 12 + k * 10, 30, 19 + k * 10, 31, 4 - k)
    pal = palette_of(*colours, GREYS["mid"])
    return "palettes at the black and white thresholds", px, pal


def v_margin():
    a = search_lum(90000, True)
    ak = lum_k(a)
    over = under = None
    for r in range(32):
        for g_ in range(32):
            for b in range(32):
                d = lum_k((r, g_, b)) - ak
                if d > MARGIN_K and (over is None or d < over[0]):
                    over = (d, (r, g_, b))
                if 0 < d < MARGIN_K and (under is None or MARGIN_K - d < under[0]):
                    under = (MARGIN_K - d, (r, g_, b))
    px = blank()
    ellipse(px, 31.5, 31.5, 27, 27, 4)
    pattern = [2, 1, 2, 3, 1, 3]
    for y in range(8, 56):
        for x in range(8, 56):
            px[y][x] = pattern[(x - 8) % 6]
    pal = palette_of(a, over[1], under[1], GREYS["black"])
    return "line margin just over and just under", px, pal


VECTORS = [
    v_small,
    v_full,
    v_rows,
    v_cols,
    v_lines,
    v_edges_cut,
    v_edges_frame,
    v_thresholds,
    v_margin,
]


# ---------------------------------------------------------------------------
# GBA formats


def to_tiles(px):
    """64 tiles of 32 bytes, row-major; four bytes a tile row, the left pixel in the low nibble."""
    out = bytearray()
    for ty in range(8):
        for tx in range(8):
            for row in range(8):
                for pair in range(4):
                    left = px[ty * 8 + row][tx * 8 + pair * 2]
                    right = px[ty * 8 + row][tx * 8 + pair * 2 + 1]
                    out.append(left | right << 4)
    return bytes(out)


def from_tiles(raw):
    px = blank()
    for t in range(64):
        for row in range(8):
            for pair in range(4):
                b = raw[t * 32 + row * 4 + pair]
                y = (t // 8) * 8 + row
                x = (t % 8) * 8 + pair * 2
                px[y][x] = b & 15
                px[y][x + 1] = b >> 4
    return px


def lz77(data):
    """GBA BIOS type 0x10, greedy: the longest match (3..18 bytes, 1..4096 back, overlapping
    allowed), else a literal. Padded with zeros to a multiple of 4."""
    n = len(data)
    out = bytearray([0x10, n & 255, n >> 8 & 255, n >> 16 & 255])
    stats = {"literals": 0, "copies": 0, "overlaps": 0}
    pos = 0
    while pos < n:
        flags = 0
        group = bytearray()
        for bit in range(8):
            if pos >= n:
                break
            best_len, best_disp = 0, 0
            if pos >= 1:
                limit = min(18, n - pos)
                for disp in range(1, min(4096, pos) + 1):
                    start = pos - disp
                    if data[start] != data[pos]:
                        continue
                    length = 1
                    while length < limit and data[start + length] == data[pos + length]:
                        length += 1
                    if length > best_len:
                        best_len, best_disp = length, disp
            if best_len >= 3:
                flags |= 0x80 >> bit
                d = best_disp - 1
                group += bytes([(best_len - 3) << 4 | d >> 8, d & 255])
                pos += best_len
                stats["copies"] += 1
                stats["overlaps"] += best_disp < best_len
            else:
                group.append(data[pos])
                pos += 1
                stats["literals"] += 1
        out.append(flags)
        out += group
    while len(out) % 4:
        out.append(0)
    return bytes(out), stats


def unlz77(stream):
    n = stream[1] | stream[2] << 8 | stream[3] << 16
    out = bytearray()
    pos = 4
    while len(out) < n:
        flags = stream[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= n:
                break
            if flags & (0x80 >> bit):
                b0, b1 = stream[pos], stream[pos + 1]
                pos += 2
                length, disp = (b0 >> 4) + 3, ((b0 & 15) << 8 | b1) + 1
                for _ in range(length):
                    out.append(out[-disp])
            else:
                out.append(stream[pos])
                pos += 1
    assert len(out) == n
    return bytes(out)


# ---------------------------------------------------------------------------
# Output


def c_bytes(name, data):
    lines = [f"static const uint8_t {name}[{len(data)}] = {{"]
    for i in range(0, len(data), 16):
        lines.append("    " + ", ".join(f"0x{b:02X}" for b in data[i : i + 16]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_words(name, words):
    body = ", ".join(f"0x{w:04X}" for w in words)
    return f"static const uint16_t {name}[{len(words)}] = {{{body}}};"


def build():
    parts = [
        "/* Generated by tools/ub_pic_vectors.py. Do not edit. */",
        "#pragma once",
        "",
        "#include <stdint.h>",
        "",
        "/* Per vector: the 64x64 4bpp tiles, the BGR555 palette, the 404-byte record",
        " * tools/gen3_sprites.py makes of them, and the tiles in GBA LZ77 (type 0x10). */",
        "",
    ]
    totals = {"literals": 0, "copies": 0, "overlaps": 0}
    names = []
    for index, make in enumerate(VECTORS):
        title, px, pal5 = make()
        check_palette(pal5, px, title)
        palette8 = [rgb8(c) for c in pal5]
        record = g.fxbm_record(g.convert(px, palette8))
        assert len(record) == 404
        tiles = to_tiles(px)
        assert from_tiles(tiles) == px
        packed, stats = lz77(tiles)
        assert unlz77(packed) == tiles
        for k in totals:
            totals[k] += stats[k]
        names.append((index, title, len(packed)))
        parts.append(f"/* Vector {index}: {title} */")
        parts.append(c_bytes(f"ub_pic_v{index}_tiles", tiles))
        parts.append(c_words(f"ub_pic_v{index}_palette", [bgr555(c) for c in pal5]))
        parts.append(c_bytes(f"ub_pic_v{index}_record", record))
        parts.append(c_bytes(f"ub_pic_v{index}_lz", packed))
        parts.append("")
    assert totals["literals"] and totals["copies"] and totals["overlaps"], totals

    # A compressed palette: 16 colours, 32 bytes, with a repeat so it holds a copy as well
    pal_words = [0x0000, 0x7FFF, 0x7FFF, 0x7FFF, 0x294A, 0x294A, 0x294A, 0x294A] + [
        0x03E0,
        0x7C00,
        0x001F,
        0x001F,
        0x001F,
        0x001F,
        0x4210,
        0x0000,
    ]
    pal_raw = b"".join(w.to_bytes(2, "little") for w in pal_words)
    pal_lz, _ = lz77(pal_raw)
    assert unlz77(pal_lz) == pal_raw and len(pal_raw) == 32
    parts.append("/* A palette of 16 colours, 32 bytes, raw and in GBA LZ77 */")
    parts.append(c_bytes("ub_pic_palette_raw", pal_raw))
    parts.append(c_bytes("ub_pic_palette_lz", pal_lz))
    parts.append("")

    parts.append("typedef struct {")
    parts.append("    const uint8_t* tiles;")
    parts.append("    const uint16_t* palette;")
    parts.append("    const uint8_t* record;")
    parts.append("    const uint8_t* tiles_lz;")
    parts.append("    uint32_t tiles_lz_size;")
    parts.append("} UbPicVector;")
    parts.append("")
    parts.append("static const UbPicVector ub_pic_vectors[] = {")
    for index, title, size in names:
        parts.append(
            f"    {{ub_pic_v{index}_tiles, ub_pic_v{index}_palette, ub_pic_v{index}_record, "
            f"ub_pic_v{index}_lz, {size}}}, /* {title} */"
        )
    parts.append("};")
    parts.append(f"#define UB_PIC_VECTOR_COUNT {len(names)}u")
    parts.append("")
    return "\n".join(parts)


def main():
    text = build()
    with open(OUTPUT, "w", newline="\n") as f:
        f.write(text)
    print(f"wrote {os.path.relpath(OUTPUT, os.path.dirname(TOOLS_DIR))} ({len(text)} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
