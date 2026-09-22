#!/usr/bin/env bash
# Render every font in a folder of .cpf files into contact sheets.
#   bash test/host/fontshots.sh [fonts-dir]     (default: sd/writer/fonts)
# Output: test/host/out/fontshots/sheet-N.png
set -euo pipefail
cd "$(dirname "$0")/../.."

FONTS=${1:-sd/writer/fonts}
OUT=test/host/out/fontshots
SDROOT=$OUT/sd
rm -rf "$OUT"
mkdir -p "$SDROOT/writer/fonts" "$SDROOT/writer/docs"
cp "$FONTS"/*.cpf "$SDROOT/writer/fonts/"

g++ -std=c++17 -O1 -Wall -static -I test/host/stubs -I src \
  test/host/fontshots.cpp test/host/stubs/stubs.cpp test/host/stubs/battery_stub.cpp \
  src/gapbuffer.cpp src/editor.cpp src/font.cpp src/settings.cpp \
  -o "$OUT/fontshots.exe"
"$OUT/fontshots.exe" "$SDROOT" "$OUT"

python - "$OUT" <<'EOF'
import os, sys
from PIL import Image, ImageDraw, ImageFont
out = sys.argv[1]
rows = [l.rstrip("\n").split("\t") for l in open(os.path.join(out, "manifest.txt"), encoding="utf-8")]
rows.sort(key=lambda r: r[1].lower())
Z, COLS, PER = 2, 3, 12          # 2x zoom, 3 columns, 12 fonts per sheet
W, H, LABEL = 240 * Z, 135 * Z, 26
try:
    label_font = ImageFont.truetype("C:/Windows/Fonts/segoeui.ttf", 16)
except OSError:
    label_font = ImageFont.load_default()
for sheet in range(0, len(rows), PER):
    chunk = rows[sheet:sheet + PER]
    nrows = (len(chunk) + COLS - 1) // COLS
    img = Image.new("RGB", (COLS * (W + 12) + 12, nrows * (H + LABEL + 12) + 12), (40, 40, 44))
    d = ImageDraw.Draw(img)
    for i, (stem, label, base, scale) in enumerate(chunk):
        x = 12 + (i % COLS) * (W + 12)
        y = 12 + (i // COLS) * (H + LABEL + 12)
        shot = Image.open(os.path.join(out, stem + ".ppm")).resize((W, H), Image.NEAREST)
        d.text((x, y + 2), f"{label}   {base}px x{scale}", fill=(235, 235, 235), font=label_font)
        img.paste(shot, (x, y + LABEL))
    path = os.path.join(out, f"sheet-{sheet // PER + 1}.png")
    img.save(path)
    print(path)
for f in os.listdir(out):
    if f.endswith(".ppm"):
        os.remove(os.path.join(out, f))
EOF
