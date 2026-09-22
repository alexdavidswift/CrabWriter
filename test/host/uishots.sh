#!/usr/bin/env bash
# Run the whole app on the PC, walk through its screens, and save screenshots.
#   bash test/host/uishots.sh
# Output: test/host/out/ui/*.png
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=test/host/out/ui
SDROOT=$OUT/sd
rm -rf "$OUT"
mkdir -p "$SDROOT/writer/fonts" "$SDROOT/writer/docs"

# A believable card: a few documents, plus up to four of your own fonts from
# fonts/ (not in the repo). Without any, the editor uses a built-in font.
FONT=builtin:0
shopt -s nullglob
n=0
for f in fonts/*.ttf fonts/*.otf fonts/*.bdf; do
  [ $n -lt 4 ] || break
  if python tools/fontconv.py "$f" -o "$SDROOT/writer/fonts" >/dev/null; then
    cpf=$(basename "${f%.*}").cpf
    [ -f "$SDROOT/writer/fonts/$cpf" ] || continue
    [ $n -eq 0 ] && FONT=$cpf
    n=$((n + 1))
  fi
done
cat > "$SDROOT/writer/docs/chapter-01.txt" <<'EOF'
Chapter One

The rain had not stopped for three days. Mara pulled her coat tighter and stepped off the train, into a town that smelled of wet stone and woodsmoke.

"You're late," said the man on the platform.
EOF
printf 'Chapter Two\n\nThe inn had one room left, and it was above the kitchen.' > "$SDROOT/writer/docs/chapter-02.txt"
printf 'Names: Mara, Tobias, the ferryman\nTown: Harrowgate\n' > "$SDROOT/writer/docs/notes.md"
printf 'theme=Night\nfont=%s\nlast_file=chapter-01.txt\n' "$FONT" > "$SDROOT/writer/settings.txt"

g++ -std=c++17 -O1 -Wall -static -I test/host/stubs -I src \
  test/host/uishots.cpp test/host/stubs/stubs.cpp test/host/stubs/app_stubs.cpp \
  src/main.cpp src/gapbuffer.cpp src/editor.cpp src/font.cpp src/settings.cpp \
  -o "$OUT/uishots.exe"
"$OUT/uishots.exe" "$SDROOT" "$OUT" >/dev/null

python - "$OUT" <<'EOF'
import glob, os, sys
from PIL import Image
for p in sorted(glob.glob(os.path.join(sys.argv[1], "*.ppm"))):
    Image.open(p).resize((720, 405), Image.NEAREST).save(p[:-4] + ".png")
    os.remove(p)
    print(p[:-4] + ".png")
EOF
