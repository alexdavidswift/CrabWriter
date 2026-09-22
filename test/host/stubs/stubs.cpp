#include <Arduino.h>
#include <M5GFX.h>
#include <SD.h>
#include <esp_heap_caps.h>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

// ------------------------------------------------------------------ clock --
static uint32_t s_now = 1000;
uint32_t millis() { return s_now; }
void hostAdvanceMillis(uint32_t ms) { s_now += ms; }

// ------------------------------------------------------------------- heap --
size_t hostHeapFree = 300 * 1024;

// ---------------------------------------------------------- other modules --
bool usbKbdStarted() { return false; }
bool usbKbdConnected() { return false; }
const char* usbKbdStatus() { return "off"; }

// --------------------------------------------------------------------- SD --
SDClass SD;
static std::string s_root = ".";

void hostSetSdRoot(const std::string& dir) { s_root = dir; }
std::string hostSdPath(const String& p) { return s_root + p.str(); }

File SDClass::open(const String& path, const char* mode) {
  std::string real = hostSdPath(path);
  std::error_code ec;
  std::string name = fs::path(real).filename().string();
  if (fs::is_directory(real, ec)) {
    std::vector<std::string> entries;
    for (auto& e : fs::directory_iterator(real, ec)) entries.push_back(e.path().filename().string());
    std::sort(entries.begin(), entries.end());
    return File(name, entries, real);
  }
  FILE* f = fopen(real.c_str(), strcmp(mode, "w") == 0 ? "wb" : "rb");
  if (!f) return File();
  size_t size = fs::exists(real, ec) ? (size_t)fs::file_size(real, ec) : 0;
  return File(f, name, size);
}

File File::openNextFile() {
  if (!_isDir || _next >= _entries.size()) return File();
  std::string p = _dirPath + "/" + _entries[_next++];
  std::error_code ec;
  if (fs::is_directory(p, ec)) return File(fs::path(p).filename().string(), {}, p);
  FILE* f = fopen(p.c_str(), "rb");
  return File(f, fs::path(p).filename().string(), (size_t)fs::file_size(p, ec));
}

bool SDClass::exists(const String& path) {
  std::error_code ec;
  return fs::exists(hostSdPath(path), ec);
}
bool SDClass::remove(const String& path) {
  std::error_code ec;
  return fs::remove(hostSdPath(path), ec);
}
bool SDClass::rename(const String& from, const String& to) {
  // FAT refuses to rename onto an existing file; mimic that.
  std::error_code ec;
  if (fs::exists(hostSdPath(to), ec)) return false;
  fs::rename(hostSdPath(from), hostSdPath(to), ec);
  return !ec;
}
bool SDClass::mkdir(const String& path) {
  std::error_code ec;
  return fs::create_directories(hostSdPath(path), ec);
}

// ---------------------------------------------------------------- display --
#define PROGMEM
#include "../../../.pio/libdeps/cardputer/M5GFX/src/lgfx/Fonts/glcdfont.h"
#include "../../../.pio/libdeps/cardputer/M5GFX/src/lgfx/Fonts/Font16.h"
#include "../../../.pio/libdeps/cardputer/M5GFX/src/lgfx/Fonts/Ascii8x16.h"

namespace fonts {
const lgfx::IFont AsciiFont8x16{8, 16, 3}, Font0{6, 8, 1}, Font8x8C64{8, 8, 0}, Font2{7, 16, 2},
    FreeSerif9pt7b{8, 18}, FreeSans9pt7b{8, 18}, FreeMono9pt7b{11, 18}, DejaVu12{7, 14},
    TomThumb{4, 6};
}

static int glyphWidth(const lgfx::IFont* f, unsigned char c) {
  if (f && f->kind == 2) return (c >= 32 && c < 128) ? widtbl_f16[c - 32] : 0;
  return f ? f->w : 6;
}

int LGFX_Sprite::textWidth(const char* s) const {
  int w = 0;
  for (; *s; s++)
    if ((*s & 0xC0) != 0x80) w += glyphWidth(_font, (unsigned char)*s) * _size;
  return w;
}

// Draws one glyph from M5GFX's real font tables; unknown kinds get a box.
static void drawGlyph(LGFX_Sprite& c, const lgfx::IFont* f, int size, int x, int y,
                      unsigned char ch, uint16_t color) {
  auto px = [&](int gx, int gy) { c.fillRect(x + gx * size, y + gy * size, size, size, color); };
  int kind = f ? f->kind : 0;
  if (kind == 1) {  // GLCD: 5 column bytes per char, bit 0 at the top
    for (int col = 0; col < 5; col++) {
      unsigned char bits = font[ch * 5 + col];
      for (int row = 0; row < 8; row++) if (bits & (1 << row)) px(col, row);
    }
  } else if (kind == 2) {  // Font2: rows of (w+6)/8 bytes, MSB left
    if (ch < 32 || ch >= 128) return;
    int w = widtbl_f16[ch - 32];
    int stride = (w + 6) >> 3;
    const unsigned char* d = chrtbl_f16[ch - 32];
    for (int row = 0; row < chr_hgt_f16; row++)
      for (int col = 0; col < w; col++)
        if (d[row * stride + (col >> 3)] & (0x80 >> (col & 7))) px(col, row);
  } else if (kind == 3) {  // 8x16: one byte per row, MSB left
    for (int row = 0; row < 16; row++) {
      unsigned char bits = FontLib8x16[ch * 16 + row];
      for (int col = 0; col < 8; col++) if (bits & (0x80 >> col)) px(col, row);
    }
  } else if (ch != ' ') {
    int cw = (f ? f->w : 6) * size, h = (f ? f->h : 8) * size;
    c.fillRect(x + size, y + h / 4, cw - 2 * size, h / 2, color);
  }
}

void LGFX_Sprite::drawString(const char* s, int x, int y) {
  int w = textWidth(s), h = fontHeight();
  if (_datum == top_right) x -= w;
  else if (_datum == top_center || _datum == middle_center) x -= w / 2;
  if (_datum == middle_center) y -= h / 2;
  else if (_datum == bottom_left) y -= h;
  for (const char* p = s; *p; p++) {
    unsigned char ch = (unsigned char)*p;
    if ((ch & 0xC0) == 0x80) continue;
    if (ch >= 0x80) ch = '?';
    drawGlyph(*this, _font, _size, x, y, ch, _color);
    x += glyphWidth(_font, ch) * _size;
  }
}

bool hostWritePPM(const LGFX_Sprite& s, const char* path) {
  FILE* f = fopen(path, "wb");
  if (!f) return false;
  fprintf(f, "P6\n%d %d\n255\n", s.width(), s.height());
  for (int y = 0; y < s.height(); y++)
    for (int x = 0; x < s.width(); x++) {
      uint16_t c = s.pixel(x, y);
      uint8_t rgb[3] = {(uint8_t)((c >> 11) << 3), (uint8_t)(((c >> 5) & 0x3F) << 2), (uint8_t)((c & 0x1F) << 3)};
      fwrite(rgb, 1, 3, f);
    }
  fclose(f);
  return true;
}
