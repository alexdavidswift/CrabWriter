#include "font.h"
#include "config.h"
#include <SD.h>
#include <algorithm>

// ---------------------------------------------------------------- helpers --

static int encodeUtf8(uint32_t cp, char* out) {
  if (cp < 0x80) { out[0] = cp; out[1] = 0; return 1; }
  if (cp < 0x800) { out[0] = 0xC0 | (cp >> 6); out[1] = 0x80 | (cp & 0x3F); out[2] = 0; return 2; }
  if (cp < 0x10000) {
    out[0] = 0xE0 | (cp >> 12); out[1] = 0x80 | ((cp >> 6) & 0x3F); out[2] = 0x80 | (cp & 0x3F);
    out[3] = 0; return 3;
  }
  out[0] = 0xF0 | (cp >> 18); out[1] = 0x80 | ((cp >> 12) & 0x3F);
  out[2] = 0x80 | ((cp >> 6) & 0x3F); out[3] = 0x80 | (cp & 0x3F); out[4] = 0;
  return 4;
}

uint32_t asciiFallback(uint32_t cp) {
  switch (cp) {
    case 0x2018: case 0x2019: case 0x201A: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0x2033: return '"';
    case 0x2013: case 0x2014: case 0x2212: return '-';
    case 0x2026: return '.';
    case 0x2022: case 0x00B7: return '*';
    case 0x00A0: return ' ';
    default: return '?';
  }
}

// ----------------------------------------------------------- BuiltinFont --

// Measuring needs an LGFX object but no pixel buffer.
static LGFX_Sprite s_measure;

void BuiltinFont::setScale(int s) {
  _scale = s < 1 ? 1 : s;
  s_measure.setFont(_font);
  s_measure.setTextSize(_scale);
  _lineH = s_measure.fontHeight();
  char buf[2] = {0, 0};
  for (int i = 0; i < 96; i++) {
    buf[0] = 32 + i;
    _asciiAdv[i] = (i == 95) ? 0 : s_measure.textWidth(buf);
  }
}

int BuiltinFont::advance(uint32_t cp) {
  if (cp < 32 || cp > 126) cp = asciiFallback(cp);
  return _asciiAdv[cp - 32];
}

void BuiltinFont::draw(LGFX_Sprite& c, int x, int y, uint32_t cp, uint16_t color) {
  if (cp <= 32 || cp > 126) {
    if (cp == ' ' || cp == 0xA0) return;
    cp = asciiFallback(cp);
  }
  char buf[5];
  encodeUtf8(cp, buf);
  c.setFont(_font);
  c.setTextSize(_scale);
  c.setTextDatum(top_left);
  c.setTextColor(color);
  c.drawString(buf, x, y);
}

// ------------------------------------------------------------ BitmapFont --

BitmapFont::~BitmapFont() { free(_data); }

bool BitmapFont::load(const char* path) {
  File f = SD.open(path, FILE_READ);
  if (!f) return false;
  size_t size = f.size();
  if (size < 44 || size > MAX_FONT_FILE) { f.close(); return false; }
  _data = (uint8_t*)malloc(size);
  if (!_data) { f.close(); return false; }
  size_t got = f.read(_data, size);
  f.close();
  if (got != size || memcmp(_data, "CPF1", 4) != 0 || _data[4] != 1) {
    free(_data);
    _data = nullptr;
    return false;
  }
  _lineH = _data[5];
  _count = _data[8] | (_data[9] << 8);
  memcpy(_name, _data + 12, 32);
  _name[32] = 0;
  size_t tableEnd = 44 + (size_t)_count * sizeof(Glyph);
  if (tableEnd > size || _count == 0) { free(_data); _data = nullptr; return false; }
  _glyphs = (const Glyph*)(_data + 44);
  _bitmaps = _data + tableEnd;
  // Reject glyphs whose bitmaps would run past the end of the file.
  size_t bmSize = size - tableEnd;
  for (int i = 0; i < _count; i++) {
    const Glyph& g = _glyphs[i];
    if (g.offset + (size_t)((g.w + 7) / 8) * g.h > bmSize) {
      free(_data);
      _data = nullptr;
      return false;
    }
  }
  return true;
}

const BitmapFont::Glyph* BitmapFont::find(uint32_t cp) const {
  int lo = 0, hi = _count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    uint32_t v = _glyphs[mid].cp;
    if (v == cp) return &_glyphs[mid];
    if (v < cp) lo = mid + 1; else hi = mid - 1;
  }
  return nullptr;
}

const BitmapFont::Glyph* BitmapFont::findOrFallback(uint32_t cp) const {
  const Glyph* g = find(cp);
  if (!g) g = find(asciiFallback(cp));
  if (!g) g = find('?');
  return g;
}

int BitmapFont::advance(uint32_t cp) {
  const Glyph* g = findOrFallback(cp);
  return g ? g->adv * _scale : 0;
}

void BitmapFont::draw(LGFX_Sprite& c, int x, int y, uint32_t cp, uint16_t color) {
  const Glyph* g = findOrFallback(cp);
  if (!g || g->w == 0) return;
  const int s = _scale;
  const int stride = (g->w + 7) / 8;
  const uint8_t* bits = _bitmaps + g->offset;
  for (int row = 0; row < g->h; row++) {
    const uint8_t* r = bits + row * stride;
    int py = y + (g->yoff + row) * s;
    int col = 0;
    while (col < g->w) {
      // Draw horizontal runs of set pixels in one call.
      if (!(r[col >> 3] & (0x80 >> (col & 7)))) { col++; continue; }
      int start = col;
      while (col < g->w && (r[col >> 3] & (0x80 >> (col & 7)))) col++;
      c.fillRect(x + (g->xoff + start) * s, py, (col - start) * s, s, color);
    }
  }
}

// ------------------------------------------------------------- catalogue --

struct BuiltinDef { const char* name; const lgfx::IFont* font; };
static const BuiltinDef kBuiltins[] = {
  {"VGA 8x16", &fonts::AsciiFont8x16},
  {"Classic 6x8", &fonts::Font0},
  {"C64 8x8", &fonts::Font8x8C64},
  {"Sans 16", &fonts::Font2},
  {"Serif 9pt", &fonts::FreeSerif9pt7b},
  {"Sans 9pt", &fonts::FreeSans9pt7b},
  {"Mono 9pt", &fonts::FreeMono9pt7b},
  {"DejaVu 12", &fonts::DejaVu12},
  {"Tom Thumb", &fonts::TomThumb},
};
static const int kBuiltinCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);

int builtinFontCount() { return kBuiltinCount; }

void fontScan(std::vector<FontEntry>& out) {
  out.clear();
  for (int i = 0; i < kBuiltinCount; i++)
    out.push_back({String("builtin:") + i, String(kBuiltins[i].name) + " (built-in)"});

  std::vector<FontEntry> sd;
  File dir = SD.open(FONTS_DIR);
  if (dir && dir.isDirectory()) {
    for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
      String fname = f.name();
      int slash = fname.lastIndexOf('/');
      if (slash >= 0) fname = fname.substring(slash + 1);
      if (!f.isDirectory() && fname.endsWith(".cpf") && fname[0] != '.') {
        // Use the display name stored in the file header when present.
        char hdr[44];
        String label = fname.substring(0, fname.length() - 4);
        if (f.read((uint8_t*)hdr, 44) == 44 && memcmp(hdr, "CPF1", 4) == 0 && hdr[12]) {
          char nm[33];
          memcpy(nm, hdr + 12, 32);
          nm[32] = 0;
          label = nm;
        }
        sd.push_back({fname, label});
      }
      f.close();
    }
    dir.close();
  }
  std::sort(sd.begin(), sd.end(), [](const FontEntry& a, const FontEntry& b) {
    return a.label.compareTo(b.label) < 0;
  });
  out.insert(out.end(), sd.begin(), sd.end());
}

Font* fontOpen(const String& id) {
  if (id.startsWith("builtin:")) {
    int i = id.substring(8).toInt();
    if (i < 0 || i >= kBuiltinCount) i = 0;
    return new BuiltinFont(kBuiltins[i].name, kBuiltins[i].font);
  }
  BitmapFont* f = new BitmapFont();
  String path = String(FONTS_DIR) + "/" + id;
  if (!f->load(path.c_str())) {
    delete f;
    return nullptr;
  }
  return f;
}
