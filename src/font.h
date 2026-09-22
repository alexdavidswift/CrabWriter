#pragma once
#include <M5GFX.h>
#include <vector>

// A text font the editor can draw with. Metrics returned already include the
// integer scale, so callers never have to think about it.
class Font {
 public:
  virtual ~Font() {}
  virtual const char* name() const = 0;
  virtual int lineHeight() const = 0;
  virtual int advance(uint32_t cp) = 0;
  virtual void draw(LGFX_Sprite& c, int x, int y, uint32_t cp, uint16_t color) = 0;
  virtual void setScale(int s) = 0;
  int scale() const { return _scale; }

 protected:
  int _scale = 1;
};

// Fonts compiled into the firmware (from M5GFX) - always available.
class BuiltinFont : public Font {
 public:
  BuiltinFont(const char* name, const lgfx::IFont* f) : _name(name), _font(f) {}
  const char* name() const override { return _name; }
  int lineHeight() const override { return _lineH; }
  int advance(uint32_t cp) override;
  void draw(LGFX_Sprite& c, int x, int y, uint32_t cp, uint16_t color) override;
  void setScale(int s) override;

 private:
  const char* _name;
  const lgfx::IFont* _font;
  int _lineH = 8;
  uint8_t _asciiAdv[96];
};

// A .cpf pixel font loaded from the SD card (see tools/fontconv.py).
class BitmapFont : public Font {
 public:
  ~BitmapFont() override;
  bool load(const char* path);
  const char* name() const override { return _name; }
  int lineHeight() const override { return _lineH * _scale; }
  int advance(uint32_t cp) override;
  void draw(LGFX_Sprite& c, int x, int y, uint32_t cp, uint16_t color) override;
  void setScale(int s) override { _scale = s < 1 ? 1 : s; }

 private:
  struct Glyph {
    uint32_t cp;
    uint32_t offset;
    uint8_t w, h;
    int8_t xoff, yoff;
    uint8_t adv;
    uint8_t pad[3];
  };
  static_assert(sizeof(Glyph) == 16, "glyph record must match file format");
  const Glyph* find(uint32_t cp) const;
  const Glyph* findOrFallback(uint32_t cp) const;

  uint8_t* _data = nullptr;
  const Glyph* _glyphs = nullptr;
  const uint8_t* _bitmaps = nullptr;
  uint16_t _count = 0;
  uint8_t _lineH = 8;
  char _name[33] = {0};
};

// Maps typographic characters to plain ASCII look-alikes when a font lacks them.
uint32_t asciiFallback(uint32_t cp);

// --- font catalogue --------------------------------------------------------

struct FontEntry {
  String id;     // "builtin:N" or file name
  String label;  // shown in menus
};

int builtinFontCount();
void fontScan(std::vector<FontEntry>& out);    // builtins + SD fonts
Font* fontOpen(const String& id);               // caller owns the result
