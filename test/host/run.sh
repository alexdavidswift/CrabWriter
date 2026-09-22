#!/usr/bin/env bash
# Build and run the host-side tests. Needs g++ (C++17) and Python with Pillow.
#   bash test/host/run.sh
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT=test/host/out
SDROOT=$OUT/sd
rm -rf "$SDROOT"
mkdir -p "$OUT" "$SDROOT/writer/fonts" "$SDROOT/writer/docs"

# Test fonts, made with the real converter (also exercises it).
python tools/fontconv.py "C:/Windows/Fonts/consola.ttf" --size 12 -o "$SDROOT/writer/fonts" >/dev/null
python tools/fontconv.py "C:/Windows/Fonts/arial.ttf" --size 11 -o "$SDROOT/writer/fonts" >/dev/null
python tools/fontconv.py test/host/fixtures/tiny.bdf -o "$SDROOT/writer/fonts" >/dev/null

g++ -std=c++17 -O1 -g -Wall -Wno-unused-function -static \
  -D_GLIBCXX_ASSERTIONS \
  -I test/host/stubs -I src \
  test/host/test_main.cpp test/host/stubs/stubs.cpp test/host/stubs/battery_stub.cpp \
  src/gapbuffer.cpp src/editor.cpp src/font.cpp src/settings.cpp \
  -o "$OUT/tests.exe" 2>&1 | grep -v "^$" || true
[ -f "$OUT/tests.exe" ] || { echo "build failed"; exit 1; }

"$OUT/tests.exe" "$SDROOT" "$OUT"
status=$?

python - "$OUT" <<'EOF'
import glob, sys, os
from PIL import Image
for p in glob.glob(os.path.join(sys.argv[1], "*.ppm")):
    Image.open(p).resize((480, 270), Image.NEAREST).save(p[:-4] + ".png")
    os.remove(p)
EOF
exit $status
