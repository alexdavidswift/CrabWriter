#!/usr/bin/env python3
"""
fontconv.py - convert pixel fonts into CrabWriter .cpf files.

Supported inputs:
  * .ttf / .otf  pixel fonts (the usual itch.io download). Rendered with
                 anti-aliasing OFF at the font's native pixel size, which is
                 auto-detected unless you pass --size.
  * .bdf         classic X11 bitmap fonts.

Usage:
  python tools/fontconv.py MyFont.ttf                      -> MyFont.cpf next to it
  python tools/fontconv.py fonts/*.ttf -o sd/writer/fonts   (batch)
  python tools/fontconv.py MyFont.ttf --size 16 --name "My Font 16"
  python tools/fontconv.py MyFont.ttf --preview             (also writes a .png preview)

Requires: Pillow  (pip install pillow). fontTools is used if installed
(pip install fonttools) for exact glyph coverage, but is optional.

.cpf format (little endian), version 1:
  0   char[4]  "CPF1"
  4   u8       version (1)
  5   u8       line height (px)
  6   u8       ascent: baseline distance from top of line (px)
  7   u8       flags (reserved, 0)
  8   u16      glyph count
  10  u16      reserved
  12  char[32] display name, NUL padded
  44  glyph table, 16 bytes per glyph, sorted by codepoint:
        u32 codepoint, u32 bitmap offset (from start of bitmap area),
        u8 width, u8 height, i8 x offset, i8 y offset (from line top),
        u8 advance, u8[3] reserved
  ... bitmap area: per glyph, rows top to bottom, each row ceil(w/8) bytes,
      MSB = leftmost pixel.
"""
import argparse
import glob
import os
import struct
import sys

# Characters worth having for prose: ASCII, Latin-1, and typographic punctuation.
DEFAULT_CHARSET = (
    list(range(0x20, 0x7F))
    + list(range(0xA0, 0x100))
    + list(range(0x100, 0x180))  # Latin Extended-A (only kept if the font has them)
    + [0x2013, 0x2014, 0x2018, 0x2019, 0x201A, 0x201C, 0x201D, 0x201E,
       0x2022, 0x2026, 0x2032, 0x2033, 0x20AC, 0x2122]
)


class Glyph:
    def __init__(self, cp, w, h, xoff, yoff, adv, rows):
        self.cp, self.w, self.h = cp, w, h
        self.xoff, self.yoff, self.adv = xoff, yoff, adv
        self.rows = rows  # list of lists of 0/1

    def packed(self):
        out = bytearray()
        for row in self.rows:
            for bx in range(0, self.w, 8):
                b = 0
                for i in range(8):
                    if bx + i < self.w and row[bx + i]:
                        b |= 0x80 >> i
                out.append(b)
        return bytes(out)


class FontData:
    def __init__(self, name, line_height, ascent, glyphs):
        self.name, self.line_height, self.ascent = name, line_height, ascent
        self.glyphs = sorted(glyphs, key=lambda g: g.cp)


# ---------------------------------------------------------------- TTF / OTF --

def _load_cmap(path):
    try:
        from fontTools.ttLib import TTFont
    except ImportError:
        return None
    try:
        tt = TTFont(path, fontNumber=0, lazy=True)
        return set(tt.getBestCmap().keys())
    except Exception:
        return None


def _crispness(font, sample="AaBbGgQq0123?!"):
    """Fraction of partially-covered pixels when rendered anti-aliased."""
    from PIL import Image, ImageDraw
    l, t, r, b = font.getbbox(sample)
    if r - l <= 0 or b - t <= 0:
        return 1.0
    img = Image.new("L", (r - l + 4, b - t + 4), 0)
    ImageDraw.Draw(img).text((2 - l, 2 - t), sample, font=font, fill=255)
    px = img.getdata()
    inked = sum(1 for p in px if p > 0)
    if inked == 0:
        return 1.0
    grey = sum(1 for p in px if 24 < p < 232)
    return grey / inked


def detect_pixel_size(path):
    """Returns (size, exact). exact=False means an approximate grid size."""
    from PIL import ImageFont
    crisp = {}
    for size in range(5, 65):
        try:
            crisp[size] = _crispness(ImageFont.truetype(path, size))
        except OSError:
            continue
        if crisp[size] < 0.02:
            return size, True

    # Pixel fonts with angled/chamfered corners are never perfectly crisp, but
    # their blur still dips sharply at every multiple of the design grid.
    # Look for a size that is a clear dip and whose double is one too.
    def dip(s):
        return (s - 1 in crisp and s + 1 in crisp and s in crisp
                and crisp[s] < 0.8 * min(crisp[s - 1], crisp[s + 1]))
    for s in range(8, 33):
        if dip(s) and dip(2 * s):
            return s, False
    return None, False


def load_ttf(path, size=None, charset=DEFAULT_CHARSET):
    from PIL import Image, ImageDraw, ImageFont

    if size is None:
        size, exact = detect_pixel_size(path)
        if size is None:
            raise ValueError("no crisp pixel size found (not a pixel font?) - pass --size N")
        if exact:
            print(f"  detected pixel size {size}px")
        else:
            print(f"  approximate pixel size {size}px (font has angled edges; check the preview"
                  f" or try --size {size * 3 // 2} / {size * 2})")
    font = ImageFont.truetype(path, size)
    ascent, descent = font.getmetrics()
    cmap = _load_cmap(path)

    def render(ch):
        l, t, r, b = font.getbbox(ch, anchor="ls")
        w, h = max(r - l, 0), max(b - t, 0)
        adv = int(round(font.getlength(ch)))
        if w == 0 or h == 0:
            return adv, None
        img = Image.new("1", (w, h), 0)
        d = ImageDraw.Draw(img)
        d.fontmode = "1"
        d.text((-l, -t), ch, font=font, fill=1, anchor="ls")
        return adv, (l, t, img)

    # Without fontTools, detect missing glyphs by comparing against .notdef.
    notdef = None
    if cmap is None:
        _, nd = render(chr(0x10FFFD))
        notdef = (nd[2].tobytes(), nd[2].size) if nd else None

    glyphs = []
    for cp in charset:
        ch = chr(cp)
        if cmap is not None and cp not in cmap and cp != 0x20:
            continue
        adv, r = render(ch)
        if r is None:
            if cp in (0x20, 0xA0) or (cmap is not None):
                glyphs.append(Glyph(cp, 0, 0, 0, 0, max(adv, 1), []))
            continue
        l, t, img = r
        if notdef and cp > 0x7E and (img.tobytes(), img.size) == notdef:
            continue
        # Trim empty rows/cols so we store only the ink.
        bbox = img.getbbox()
        if bbox is None:
            glyphs.append(Glyph(cp, 0, 0, 0, 0, max(adv, 1), []))
            continue
        img = img.crop(bbox)
        w, h = img.size
        px = img.load()
        rows = [[1 if px[x, y] else 0 for x in range(w)] for y in range(h)]
        xoff = l + bbox[0]
        yoff = ascent + t + bbox[1]  # t is relative to baseline (negative = above)
        glyphs.append(Glyph(cp, w, h, xoff, yoff, adv, rows))

    name = " ".join(x for x in font.getname() if x and x.lower() != "regular")
    return FontData(f"{name} {size}", ascent + descent, ascent, glyphs)


# ---------------------------------------------------------------------- BDF --

def load_bdf(path, charset=DEFAULT_CHARSET):
    wanted = set(charset)
    glyphs = []
    name = os.path.splitext(os.path.basename(path))[0]
    font_ascent = font_descent = None
    fbb = (0, 0, 0, 0)
    with open(path, "r", encoding="latin-1") as f:
        lines = [ln.rstrip("\n") for ln in f]
    i = 0
    while i < len(lines):
        parts = lines[i].split()
        if not parts:
            i += 1
            continue
        kw = parts[0]
        if kw == "FONTBOUNDINGBOX":
            fbb = tuple(int(x) for x in parts[1:5])
        elif kw == "FONT_ASCENT":
            font_ascent = int(parts[1])
        elif kw == "FONT_DESCENT":
            font_descent = int(parts[1])
        elif kw == "FAMILY_NAME":
            name = lines[i].split(None, 1)[1].strip().strip('"')
        elif kw == "STARTCHAR":
            enc, dwidth, bbx, bitmap = -1, None, None, []
            i += 1
            while i < len(lines) and not lines[i].startswith("ENDCHAR"):
                p = lines[i].split()
                if p and p[0] == "ENCODING":
                    enc = int(p[1])
                elif p and p[0] == "DWIDTH":
                    dwidth = int(p[1])
                elif p and p[0] == "BBX":
                    bbx = tuple(int(x) for x in p[1:5])
                elif p and p[0] == "BITMAP":
                    i += 1
                    while i < len(lines) and not lines[i].startswith("ENDCHAR"):
                        bitmap.append(lines[i].strip())
                        i += 1
                    break
                i += 1
            if enc in wanted and bbx is not None:
                w, h, bx, by = bbx
                rows = []
                for hexrow in bitmap[:h]:
                    v = int(hexrow, 16) if hexrow else 0
                    nbits = len(hexrow) * 4
                    rows.append([(v >> (nbits - 1 - x)) & 1 for x in range(w)])
                g = Glyph(enc, w, h, bx, 0, dwidth if dwidth is not None else w, rows)
                glyphs.append((g, by))
        i += 1
    if font_ascent is None:
        font_ascent = fbb[1] + fbb[3]
    if font_descent is None:
        font_descent = -fbb[3]
    out = []
    for g, by in glyphs:
        # BDF y offset is the bottom of the bbox relative to the baseline.
        g.yoff = font_ascent - (by + g.h)
        out.append(g)
    return FontData(name, font_ascent + font_descent, font_ascent, out)


# ------------------------------------------------------------------- output --

def write_cpf(fd, out_path):
    glyphs = fd.glyphs
    if not glyphs:
        raise ValueError("no glyphs found")
    for g in glyphs:
        if not (0 <= g.w < 256 and 0 <= g.h < 256 and 0 <= g.adv < 256):
            raise ValueError(f"glyph U+{g.cp:04X} too large for format")
        g.xoff = max(-128, min(127, g.xoff))
        g.yoff = max(-128, min(127, g.yoff))
    line_height = max(1, min(255, fd.line_height))
    ascent = max(0, min(255, fd.ascent))

    table = bytearray()
    bitmaps = bytearray()
    for g in glyphs:
        data = g.packed()
        table += struct.pack("<IIBBbbB3x", g.cp, len(bitmaps), g.w, g.h, g.xoff, g.yoff, g.adv)
        bitmaps += data
    name = fd.name.encode("utf-8")[:31]
    header = struct.pack("<4sBBBBHH32s", b"CPF1", 1, line_height, ascent, 0,
                         len(glyphs), 0, name)
    with open(out_path, "wb") as f:
        f.write(header + table + bitmaps)
    return len(header) + len(table) + len(bitmaps)


def write_preview(fd, png_path, scale=3):
    from PIL import Image
    text = ["The quick brown fox jumps", "over the lazy dog. 0123456789",
            "“Curly quotes” — café à üñ …"]
    by_cp = {g.cp: g for g in fd.glyphs}
    width = max(sum(by_cp.get(ord(c), by_cp.get(0x3F)).adv for c in t if by_cp.get(ord(c), by_cp.get(0x3F)))
                for t in text) + 8
    height = fd.line_height * len(text) + 8
    img = Image.new("1", (width, height), 0)
    px = img.load()
    y = 4
    for t in text:
        x = 4
        for c in t:
            g = by_cp.get(ord(c)) or by_cp.get(0x3F)
            if not g:
                continue
            for ry, row in enumerate(g.rows):
                for rx, v in enumerate(row):
                    X, Y = x + g.xoff + rx, y + g.yoff + ry
                    if v and 0 <= X < width and 0 <= Y < height:
                        px[X, Y] = 1
            x += g.adv
        y += fd.line_height
    img = img.resize((width * scale, height * scale), Image.NEAREST)
    img.save(png_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("inputs", nargs="+", help="font files (.ttf .otf .bdf), globs allowed")
    ap.add_argument("-o", "--out", help="output directory (default: next to input)")
    ap.add_argument("--size", type=int, help="pixel size for TTF/OTF (default: auto-detect)")
    ap.add_argument("--name", help="display name shown on the device")
    ap.add_argument("--preview", action="store_true", help="also write a preview .png")
    args = ap.parse_args()

    paths = []
    for p in args.inputs:
        paths.extend(glob.glob(p) or [p])
    ok = 0
    for path in paths:
        ext = os.path.splitext(path)[1].lower()
        print(f"{path}")
        try:
            if ext in (".ttf", ".otf"):
                fd = load_ttf(path, args.size)
            elif ext == ".bdf":
                fd = load_bdf(path)
            else:
                print("  skipped: unsupported file type")
                continue
            if args.name:
                fd.name = args.name
            base = os.path.splitext(os.path.basename(path))[0]
            if args.size:
                base += f"-{args.size}"
            out_dir = args.out or os.path.dirname(path) or "."
            os.makedirs(out_dir, exist_ok=True)
            out = os.path.join(out_dir, base + ".cpf")
            size = write_cpf(fd, out)
            print(f"  -> {out}  ({len(fd.glyphs)} glyphs, line {fd.line_height}px, {size} bytes)")
            if args.preview:
                write_preview(fd, os.path.splitext(out)[0] + ".png")
            ok += 1
        except Exception as e:  # keep going in batch mode
            print(f"  failed: {e}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
