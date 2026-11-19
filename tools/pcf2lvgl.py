#!/usr/bin/env python3
"""Converts an X11 PCF bitmap font (.pcf or .pcf.gz) into an LVGL 9 font in C.

The output is a 1bpp lv_font_fmt_txt font: pixels stay exactly as drawn in
the bitmap font, with no scaling and no anti-aliasing. lv_font_conv only
reads outline fonts (and needs Node.js), hence this script.

Usage:
    tools/pcf2lvgl.py SOURCE.pcf.gz NAME OUTPUT.c [--ranges 0x20-0x7e,0xa0-0x17f,...]

Only standard Python. PCF layout as in FreeType's pcfread.c and the X11
"pcf" format notes.
"""

import argparse
import gzip
import struct
import sys

PCF_ACCELERATORS = 1 << 1
PCF_METRICS = 1 << 2
PCF_BITMAPS = 1 << 3
PCF_BDF_ENCODINGS = 1 << 5
PCF_BDF_ACCELERATORS = 1 << 8

PCF_GLYPH_PAD_MASK = 3
PCF_BYTE_MASK = 1 << 2  # set: most significant byte first
PCF_BIT_MASK = 1 << 3  # set: most significant bit first
PCF_SCAN_UNIT_MASK = 3 << 4
PCF_COMPRESSED_METRICS = 0x100

# Latin (basic, Latin-1, Extended-A), Greek, Cyrillic, common punctuation.
DEFAULT_RANGES = "0x20-0x7e,0xa0-0x17f,0x370-0x3ff,0x400-0x4ff,0x2010-0x2027,0x2030-0x203a,0x20ac,0x2122"


class Reader:
    def __init__(self, data, offset, fmt):
        self.data = data
        self.pos = offset
        self.endian = ">" if fmt & PCF_BYTE_MASK else "<"

    def _unpack(self, code, size):
        (v,) = struct.unpack_from(self.endian + code, self.data, self.pos)
        self.pos += size
        return v

    def i32(self):
        return self._unpack("i", 4)

    def i16(self):
        return self._unpack("h", 2)

    def u16(self):
        return self._unpack("H", 2)

    def u8(self):
        v = self.data[self.pos]
        self.pos += 1
        return v


def open_table(data, toc, kind):
    """Returns a Reader positioned after the table's format word, and the format."""
    if kind not in toc:
        return None, 0
    offset = toc[kind]
    (fmt,) = struct.unpack_from("<i", data, offset)  # the format word is always LSB first
    return Reader(data, offset + 4, fmt), fmt


def read_metric(r, compressed):
    if compressed:
        lsb, rsb, width, ascent, descent = (r.u8() - 0x80 for _ in range(5))
    else:
        lsb, rsb, width, ascent, descent = (r.i16() for _ in range(5))
        r.u16()  # attributes
    return lsb, rsb, width, ascent, descent


def parse_pcf(path):
    raw = open(path, "rb").read()
    data = gzip.decompress(raw) if raw[:2] == b"\x1f\x8b" else raw
    if data[:4] != b"\x01fcp":
        sys.exit(f"{path}: not a PCF font")
    (count,) = struct.unpack_from("<i", data, 4)
    toc = {}
    for i in range(count):
        kind, _fmt, _size, offset = struct.unpack_from("<4i", data, 8 + 16 * i)
        toc[kind] = offset

    # Font ascent and descent (the line box).
    r, fmt = open_table(data, toc, PCF_BDF_ACCELERATORS)
    if r is None:
        r, fmt = open_table(data, toc, PCF_ACCELERATORS)
    r.pos += 8  # noOverlap .. padding flags
    font_ascent = r.i32()
    font_descent = r.i32()

    r, fmt = open_table(data, toc, PCF_METRICS)
    compressed = bool(fmt & PCF_COMPRESSED_METRICS)
    n = r.i16() if compressed else r.i32()
    metrics = [read_metric(r, compressed) for _ in range(n)]

    r, fmt = open_table(data, toc, PCF_BITMAPS)
    n_bitmaps = r.i32()
    offsets = [r.i32() for _ in range(n_bitmaps)]
    sizes = [r.i32() for _ in range(4)]
    pad = 1 << (fmt & PCF_GLYPH_PAD_MASK)
    blob = bytearray(data[r.pos : r.pos + sizes[fmt & PCF_GLYPH_PAD_MASK]])
    # Normalise to most significant bit first within bytes in reading order.
    if not fmt & PCF_BIT_MASK:
        blob = bytearray(int(f"{b:08b}"[::-1], 2) for b in blob)
    unit = 1 << ((fmt & PCF_SCAN_UNIT_MASK) >> 4)
    if bool(fmt & PCF_BYTE_MASK) != bool(fmt & PCF_BIT_MASK) and unit > 1:
        for i in range(0, len(blob), unit):
            blob[i : i + unit] = blob[i : i + unit][::-1]

    glyphs = []
    for i, (lsb, rsb, width, ascent, descent) in enumerate(metrics):
        w, h = rsb - lsb, ascent + descent
        row_bytes = ((w + 7) // 8 + pad - 1) // pad * pad
        rows = []
        for y in range(h):
            start = offsets[i] + y * row_bytes
            bits = "".join(f"{b:08b}" for b in blob[start : start + row_bytes])
            rows.append(bits[:w])
        glyphs.append({"lsb": lsb, "w": w, "h": h, "adv": width, "descent": descent, "rows": rows,
                       "ascent": ascent})

    r, fmt = open_table(data, toc, PCF_BDF_ENCODINGS)
    min2, max2, min1, max1, _default = (r.i16() for _ in range(5))
    encoding = {}
    for b1 in range(min1, max1 + 1):
        for b2 in range(min2, max2 + 1):
            idx = r.u16()
            if idx != 0xFFFF:
                encoding[(b1 << 8) | b2] = idx

    return font_ascent, font_descent, glyphs, encoding


def parse_ranges(text):
    cps = set()
    for part in text.split(","):
        lo, _, hi = part.partition("-")
        cps.update(range(int(lo, 0), int(hi or lo, 0) + 1))
    return sorted(cps)


def runs(codepoints):
    """Groups sorted codepoints into runs of consecutive values."""
    out = []
    for cp in codepoints:
        if out and cp == out[-1][-1] + 1:
            out[-1].append(cp)
        else:
            out.append([cp])
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("source")
    ap.add_argument("name", help="C name of the font, e.g. ui_font_7x14b")
    ap.add_argument("output")
    ap.add_argument("--ranges", default=DEFAULT_RANGES)
    args = ap.parse_args()

    ascent, descent, glyphs, encoding = parse_pcf(args.source)
    wanted = [cp for cp in parse_ranges(args.ranges) if cp in encoding]

    bitmap = []
    dscs = ["    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0} /* id 0 reserved */"]
    for cp in wanted:
        g = glyphs[encoding[cp]]
        bits = "".join(g["rows"])
        bits += "0" * (-len(bits) % 8)
        index = len(bitmap)
        bitmap.extend(int(bits[i : i + 8], 2) for i in range(0, len(bits), 8))
        dscs.append(f"    {{.bitmap_index = {index}, .adv_w = {g['adv'] * 16}, .box_w = {g['w']}, "
                    f".box_h = {g['h']}, .ofs_x = {g['lsb']}, .ofs_y = {-g['descent']}}} /* U+{cp:04X} */")

    cmaps = []
    glyph_id = 1
    for run in runs(wanted):
        cmaps.append(f"    {{.range_start = {run[0]}, .range_length = {len(run)}, .glyph_id_start = {glyph_id}, "
                     ".unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0, "
                     ".type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY}")
        glyph_id += len(run)

    def glyph_ascent(ch):
        cp = ord(ch)
        return glyphs[encoding[cp]]["ascent"] if cp in encoding else ascent

    lines = []
    lines.append(f"/* Generated by tools/pcf2lvgl.py from {args.source.split('/')[-1]}. Do not edit.")
    lines.append(f" * {len(wanted)} glyphs, 1 bpp, ranges {args.ranges}. */")
    lines.append("")
    lines.append('#include "lvgl.h"')
    lines.append("")
    lines.append("static const uint8_t glyph_bitmap[] = {")
    for i in range(0, len(bitmap), 16):
        lines.append("    " + ", ".join(f"0x{b:02x}" for b in bitmap[i : i + 16]) + ",")
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    lines.append(",\n".join(dscs))
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    lines.append(",\n".join(cmaps))
    lines.append("};")
    lines.append("")
    lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    lines.append("    .glyph_bitmap = glyph_bitmap,")
    lines.append("    .glyph_dsc = glyph_dsc,")
    lines.append("    .cmaps = cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append(f"    .cmap_num = {len(cmaps)},")
    lines.append("    .bpp = 1,")
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = LV_FONT_FMT_TXT_PLAIN,")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_font_t {args.name} = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append(f"    .line_height = {ascent + descent},")
    lines.append(f"    .base_line = {descent},")
    lines.append(f"    .cap_height = {glyph_ascent('H')},")
    lines.append(f"    .x_height = {glyph_ascent('x')},")
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -1,")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .static_bitmap = 1,")
    lines.append("    .dsc = &font_dsc,")
    lines.append("    .fallback = NULL,")
    lines.append("    .user_data = NULL,")
    lines.append("};")
    open(args.output, "w").write("\n".join(lines) + "\n")
    print(f"{args.output}: {len(wanted)} glyphs, {len(cmaps)} ranges, {len(bitmap)} bitmap bytes, "
          f"line height {ascent + descent}")


if __name__ == "__main__":
    main()
