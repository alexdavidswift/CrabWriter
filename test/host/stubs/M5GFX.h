// Host-side display: an in-memory RGB565 framebuffer with just enough of the
// LovyanGFX API for the editor and fonts. Built-in fonts are replaced by
// fixed-size placeholders that draw each glyph as a solid box.
#pragma once
#include <Arduino.h>
#include <cmath>
#include <vector>

enum textdatum_t { top_left, top_center, top_right, middle_center, bottom_left };

#define TFT_BLACK 0x0000
#define TFT_WHITE 0xFFFF

namespace lgfx {
// kind: 0 = placeholder boxes, 1 = GLCD (column bytes), 2 = Font2 (proportional
// BMP), 3 = fixed-width row bytes. Real glyph data comes from M5GFX's own tables.
struct IFont {
  int w, h;
  int kind = 0;
};
}  // namespace lgfx

namespace fonts {
extern const lgfx::IFont AsciiFont8x16, Font0, Font8x8C64, Font2, FreeSerif9pt7b, FreeSans9pt7b,
    FreeMono9pt7b, DejaVu12, TomThumb;
}

class LGFX_Sprite {
 public:
  bool createSprite(int w, int h) {
    _w = w;
    _h = h;
    _px.assign(w * h, 0);
    return true;
  }
  int width() const { return _w; }
  int height() const { return _h; }
  uint16_t pixel(int x, int y) const { return (x < 0 || y < 0 || x >= _w || y >= _h) ? 0 : _px[y * _w + x]; }

  void fillScreen(uint16_t c) { std::fill(_px.begin(), _px.end(), c); }
  void fillRect(int x, int y, int w, int h, uint16_t c) {
    fillRects++;
    for (int yy = std::max(0, y); yy < std::min(_h, y + h); yy++)
      for (int xx = std::max(0, x); xx < std::min(_w, x + w); xx++) _px[yy * _w + xx] = c;
  }
  void drawFastHLine(int x, int y, int w, uint16_t c) { fillRect(x, y, w, 1, c); }

  void setFont(const lgfx::IFont* f) { _font = f; }
  void setTextSize(int s) { _size = s; }
  void setTextDatum(textdatum_t d) { _datum = d; }
  void setTextColor(uint16_t c) { _color = c; }
  int fontHeight() const { return _font ? _font->h * _size : 8; }
  int textWidth(const char* s) const;
  int textWidth(const String& s) const { return textWidth(s.c_str()); }
  void drawString(const char* s, int x, int y);
  void drawString(const String& s, int x, int y) { drawString(s.c_str(), x, y); }
  void drawRect(int x, int y, int w, int h, uint16_t c) {
    fillRect(x, y, w, 1, c);
    fillRect(x, y + h - 1, w, 1, c);
    fillRect(x, y, 1, h, c);
    fillRect(x + w - 1, y, 1, h, c);
  }
  void fillRoundRect(int x, int y, int w, int h, int r, uint16_t c) {
    fillRect(x + r, y, w - 2 * r, h, c);
    fillRect(x, y + r, w, h - 2 * r, c);
    for (int i = 0; i < r; i++) {  // approximate corners
      int inset = r - (int)sqrtf((float)(r * r - (r - i) * (r - i)));
      fillRect(x + inset, y + i, w - 2 * inset, 1, c);
      fillRect(x + inset, y + h - 1 - i, w - 2 * inset, 1, c);
    }
  }
  void setTextWrap(bool) {}
  void setColorDepth(int) {}

  int fillRects = 0;  // for tests

 private:
  int _w = 0, _h = 0;
  std::vector<uint16_t> _px;
  const lgfx::IFont* _font = nullptr;
  int _size = 1;
  textdatum_t _datum = top_left;
  uint16_t _color = 0xFFFF;
};

// Save the framebuffer as a binary PPM (converted to PNG by the test script).
bool hostWritePPM(const LGFX_Sprite& s, const char* path);
