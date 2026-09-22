#include "editor.h"
#include "config.h"
#include "settings.h"
#include "usb_kbd.h"
#include <SD.h>

int batteryLevel();  // cached, from main.cpp

static const size_t NPOS = (size_t)-1;

static bool isSpace(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == 0xA0; }

// ------------------------------------------------------------- file I/O --

bool Editor::open(const String& fileName) {
  close();
  _fileName = fileName;
  String path = String(DOCS_DIR) + "/" + fileName;
  if (SD.exists(path)) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t size = f.size();
    if (!_buf.reserve(size + 2048)) {
      f.close();
      flash("File too large for memory", 4000);
      return false;
    }
    static uint8_t chunk[1024];
    bool first = true;
    while (f.available()) {
      int n = f.read(chunk, sizeof(chunk));
      if (n <= 0) break;
      int start = 0;
      if (first && n >= 3 && chunk[0] == 0xEF && chunk[1] == 0xBB && chunk[2] == 0xBF) start = 3;  // BOM
      first = false;
      char* dst = _buf.appendSpace(n);
      if (!dst) break;
      int w = 0;
      for (int i = start; i < n; i++)
        if (chunk[i] != '\r') dst[w++] = chunk[i];  // normalise CRLF
      _buf.commitAppend(w);
    }
    f.close();
  }
  _open = true;
  _dirty = false;
  _cursor = _buf.length();  // resume at the end, where you left off writing
  _viewTop = 0;
  _goalX = -1;
  _wordsDirty = true;
  _lastSave = millis();
  relayout();
  return true;
}

bool Editor::save() {
  if (!_open) return false;
  String path = String(DOCS_DIR) + "/" + _fileName;
  String tmp = path + ".tmp";
  String bak = path + ".bak";
  File f = SD.open(tmp, FILE_WRITE);
  bool ok = (bool)f;
  if (ok) {
    size_t a = _buf.part1Len(), b = _buf.part2Len();
    ok = (a == 0 || f.write((const uint8_t*)_buf.part1(), a) == a) &&
         (b == 0 || f.write((const uint8_t*)_buf.part2(), b) == b);
    f.close();
  }
  // Swap in the new file only once it is fully written, keeping one backup.
  if (ok) {
    if (SD.exists(path)) {
      SD.remove(bak);
      SD.rename(path, bak);
    }
    ok = SD.rename(tmp, path);
  }
  _saveFailed = !ok;
  if (ok) _dirty = false;
  _lastSave = millis();
  return ok;
}

void Editor::close() {
  _buf.clear();
  _open = false;
  _dirty = false;
  _cursor = _viewTop = 0;
  _fileName = "";
}

// --------------------------------------------------------------- layout --

void Editor::setFont(Font* f) {
  _font = f;
  relayout();
}

void Editor::geometry() {
  if (!_font) return;
  _font->setScale(settings.scale);
  _tx = settings.marginX;
  _ty = settings.marginY;
  _tw = SCREEN_W - 2 * settings.marginX;
  _th = SCREEN_H - 2 * settings.marginY - (settings.statusBar ? STATUS_H : 0);
  if (_tw < 20) _tw = 20;
  _lineAdv = _font->lineHeight() + settings.lineSpacing;
  if (_lineAdv < 1) _lineAdv = 1;
  // A partially visible last line is still worth showing if most of it fits.
  _visible = (_th + settings.lineSpacing) / _lineAdv;
  if (_visible < 1) _visible = 1;
  _spaceW = _font->advance(' ') + settings.letterSpacing;
}

void Editor::relayout() {
  if (!_font) return;  // nothing can be laid out until a font is set
  geometry();
  if (_open) ensureVisible();
}

int Editor::charWidth(uint32_t cp) {
  if (cp == '\t') return _spaceW * 4;
  return _font->advance(cp) + settings.letterSpacing;
}

Editor::Line Editor::layout(size_t start) {
  Line L{start, start, start, false, false};
  const size_t n = _buf.length();
  int x = 0;
  size_t lastBreak = NPOS;
  size_t i = start;
  while (i < n) {
    int len;
    uint32_t cp = _buf.decode(i, &len);
    if (cp == '\n') {
      L.end = i;
      L.next = i + 1;
      L.hard = true;
      return L;
    }
    int w = charWidth(cp);
    bool sp = (cp == ' ' || cp == '\t');
    // Spaces may hang past the margin; any other glyph that overflows wraps
    // at the last space, or mid-word if the word is wider than the line.
    if (!sp && x + w > _tw && i > start) {
      L.end = L.next = (lastBreak != NPOS) ? lastBreak : i;
      return L;
    }
    x += w;
    i += len;
    if (sp) lastBreak = i;
  }
  L.end = L.next = n;
  L.eof = true;
  return L;
}

size_t Editor::paraStart(size_t pos) {
  while (pos > 0 && _buf.at(pos - 1) != '\n') pos--;
  return pos;
}

size_t Editor::lineStartFor(size_t pos) {
  size_t n = _buf.length();
  if (pos > n) pos = n;
  size_t p = paraStart(pos);
  for (;;) {
    Line L = layout(p);
    if (pos < L.next || L.eof || L.next <= p) return L.start;
    p = L.next;
  }
}

size_t Editor::backUp(size_t lineStart, int lines) {
  for (int k = 0; k < lines && lineStart > 0; k++) lineStart = lineStartFor(lineStart - 1);
  return lineStart;
}

size_t Editor::lineEndPos(const Line& L) {
  if (L.eof || L.hard) return L.end;
  // Soft-wrapped: the position at L.next belongs to the following line.
  return L.next > L.start ? _buf.prevChar(L.next) : L.next;
}

int Editor::xOf(size_t lineStart, size_t pos) {
  int x = 0;
  for (size_t i = lineStart; i < pos;) {
    int len;
    uint32_t cp = _buf.decode(i, &len);
    if (cp == '\n') break;
    x += charWidth(cp);
    i += len;
  }
  return x;
}

size_t Editor::posAtX(size_t lineStart, int x) {
  Line L = layout(lineStart);
  size_t limit = lineEndPos(L);
  int acc = 0;
  for (size_t i = lineStart; i < limit;) {
    int len;
    int w = charWidth(_buf.decode(i, &len));
    if (acc + w / 2 >= x) return i;
    acc += w;
    i += len;
  }
  return limit;
}

void Editor::ensureVisible() {
  size_t n = _buf.length();
  if (_cursor > n) _cursor = n;
  size_t cl = lineStartFor(_cursor);
  if (settings.typewriter) {
    _viewTop = backUp(cl, (_visible - 1) / 2);
    return;
  }
  _viewTop = lineStartFor(_viewTop);
  if (cl < _viewTop) {
    _viewTop = cl;
    return;
  }
  size_t p = _viewTop;
  for (int k = 0; k < _visible; k++) {
    if (p == cl) return;  // on screen
    Line L = layout(p);
    if (L.eof) break;
    p = L.next;
  }
  _viewTop = backUp(cl, _visible - 1);
}

// -------------------------------------------------------------- editing --

void Editor::insertText(const char* s, size_t n) {
  if (!_buf.insert(_cursor, s, n)) {
    flash("Memory full - start a new file", 3000);
    return;
  }
  if (_cursor < _viewTop) _viewTop += n;
  _cursor += n;
  _dirty = true;
  _wordsDirty = true;
  _lastEdit = millis();
}

void Editor::eraseRange(size_t from, size_t to) {
  if (to <= from) return;
  _buf.erase(from, to - from);
  if (_viewTop > from) _viewTop = (_viewTop >= to) ? _viewTop - (to - from) : from;
  _cursor = from;
  _dirty = true;
  _wordsDirty = true;
  _lastEdit = millis();
}

size_t Editor::wordLeft(size_t pos) {
  int len;
  while (pos > 0) {  // skip whitespace
    size_t p = _buf.prevChar(pos);
    if (!isSpace(_buf.decode(p, &len))) break;
    pos = p;
  }
  while (pos > 0) {  // then the word
    size_t p = _buf.prevChar(pos);
    if (isSpace(_buf.decode(p, &len))) break;
    pos = p;
  }
  return pos;
}

size_t Editor::wordRight(size_t pos) {
  size_t n = _buf.length();
  int len;
  while (pos < n && !isSpace(_buf.decode(pos, &len))) pos += len;
  while (pos < n && isSpace(_buf.decode(pos, &len))) pos += len;
  return pos;
}

void Editor::moveVertical(int lines) {
  size_t cl = lineStartFor(_cursor);
  if (_goalX < 0) _goalX = xOf(cl, _cursor);
  size_t n = _buf.length();
  while (lines < 0) {
    if (cl == 0) { _cursor = 0; return; }
    cl = lineStartFor(cl - 1);
    lines++;
  }
  while (lines > 0) {
    Line L = layout(cl);
    if (L.eof) { _cursor = n; return; }
    cl = L.next;
    lines--;
  }
  _cursor = posAtX(cl, _goalX);
}

void Editor::handleKey(const KeyEvent& ev) {
  if (!_open || !_font) return;
  bool ctrl = ev.mods & MOD_CTRL;
  bool vertical = false;
  size_t n = _buf.length();

  switch (ev.key) {
    case K_CHAR: {
      if (ctrl) return;  // shortcuts are handled by the app
      char tmp[5];
      uint32_t cp = ev.ch;
      size_t len;
      if (cp < 0x80) { tmp[0] = cp; len = 1; }
      else if (cp < 0x800) { tmp[0] = 0xC0 | (cp >> 6); tmp[1] = 0x80 | (cp & 0x3F); len = 2; }
      else { tmp[0] = 0xE0 | (cp >> 12); tmp[1] = 0x80 | ((cp >> 6) & 0x3F); tmp[2] = 0x80 | (cp & 0x3F); len = 3; }
      insertText(tmp, len);
      break;
    }
    case K_ENTER: insertText("\n", 1); break;
    case K_TAB: insertText("\t", 1); break;
    case K_BACKSPACE:
      if (_cursor > 0) eraseRange(ctrl ? wordLeft(_cursor) : _buf.prevChar(_cursor), _cursor);
      break;
    case K_DELETE:
      if (_cursor < n) eraseRange(_cursor, ctrl ? wordRight(_cursor) : _buf.nextChar(_cursor));
      break;
    case K_LEFT: _cursor = ctrl ? wordLeft(_cursor) : _buf.prevChar(_cursor); break;
    case K_RIGHT: _cursor = ctrl ? wordRight(_cursor) : _buf.nextChar(_cursor); break;
    case K_UP: moveVertical(-1); vertical = true; break;
    case K_DOWN: moveVertical(1); vertical = true; break;
    case K_PGUP: moveVertical(-(_visible - 1 > 0 ? _visible - 1 : 1)); vertical = true; break;
    case K_PGDN: moveVertical(_visible - 1 > 0 ? _visible - 1 : 1); vertical = true; break;
    case K_HOME:
      _cursor = ctrl ? 0 : lineStartFor(_cursor);
      break;
    case K_END:
      _cursor = ctrl ? n : lineEndPos(layout(lineStartFor(_cursor)));
      break;
    default: return;
  }
  if (!vertical) _goalX = -1;
  ensureVisible();
}

// ------------------------------------------------------------ rendering --

void Editor::flash(const String& msg, uint32_t ms) {
  _msg = msg;
  _msgUntil = millis() + ms;
}

size_t Editor::wordCount() {
  // Counting is O(n); throttle it while typing.
  if (_wordsDirty && (millis() - _wordsAt > 1500 || _buf.length() < 20000)) {
    size_t words = 0, n = _buf.length();
    bool inWord = false;
    for (size_t i = 0; i < n; i++) {
      char c = _buf.at(i);
      bool sp = c == ' ' || c == '\n' || c == '\t';
      if (!sp && !inWord) words++;
      inWord = !sp;
    }
    _words = words;
    _wordsDirty = false;
    _wordsAt = millis();
  }
  return _words;
}

void Editor::render(LGFX_Sprite& c, bool cursorOn) {
  Theme th = themeAt(settings.theme);
  uint16_t bg = rgb565(th.bg), fg = rgb565(th.fg), accent = rgb565(th.accent), dim = rgb565(th.dim);
  c.fillScreen(bg);
  if (!_font) return;

  const int lh = _font->lineHeight();
  const int s = _font->scale();
  int y = _ty;
  size_t p = _viewTop;
  int curX = -1, curY = 0, curW = _spaceW;
  uint32_t curCp = ' ';

  for (int k = 0; k < _visible; k++) {
    Line L = layout(p);
    int x = _tx;
    bool cursorHere = _cursor >= L.start && (_cursor < L.next || L.eof);
    for (size_t i = L.start; i < L.end;) {
      int len;
      uint32_t cp = _buf.decode(i, &len);
      int w = charWidth(cp);
      if (i == _cursor) { curX = x; curY = y; curW = w; curCp = cp; }
      if (!isSpace(cp)) _font->draw(c, x, y, cp, fg);
      x += w;
      i += len;
    }
    if (cursorHere && curX < 0) { curX = x; curY = y; }
    if (L.eof) break;
    p = L.next;
    y += _lineAdv;
  }

  if (cursorOn && curX >= 0) {
    if (curX > SCREEN_W - 2) curX = SCREEN_W - 2;
    if (curW < s * 2) curW = _spaceW > 0 ? _spaceW : s * 4;
    switch (settings.cursorStyle) {
      case 1:  // block, with the character redrawn inverted
        c.fillRect(curX, curY, curW, lh, accent);
        if (!isSpace(curCp)) _font->draw(c, curX, curY, curCp, bg);
        break;
      case 2:  // underline
        c.fillRect(curX, curY + lh - s, curW, s, accent);
        break;
      default:  // bar
        c.fillRect(curX, curY, s > 1 ? s : 1, lh, accent);
        break;
    }
  }

  if (!settings.statusBar) {
    // Still show transient messages, as a small overlay.
    if (millis() < _msgUntil) {
      c.setFont(&fonts::Font0);
      c.setTextSize(1);
      int w = c.textWidth(_msg) + 6;
      c.fillRect(SCREEN_W - w, SCREEN_H - 10, w, 10, accent);
      c.setTextColor(bg);
      c.setTextDatum(top_left);
      c.drawString(_msg, SCREEN_W - w + 3, SCREEN_H - 9);
    }
    return;
  }

  // Status bar
  int sy = SCREEN_H - STATUS_H;
  c.drawFastHLine(0, sy, SCREEN_W, dim);
  c.setFont(&fonts::Font0);
  c.setTextSize(1);
  c.setTextDatum(top_left);
  String left;
  if (millis() < _msgUntil) {
    left = _msg;
    c.setTextColor(accent);
  } else {
    left = _fileName;
    if (_dirty) left += " *";
    if (_saveFailed) left += " SAVE FAILED";
    c.setTextColor(_saveFailed ? accent : dim);
  }
  c.drawString(left, 3, sy + 2);

  char right[48];
  int bat = batteryLevel();
  const char* usb = usbKbdStarted() ? (usbKbdConnected() ? " USB" : " usb?") : "";
  if (bat >= 0 && bat <= 100)
    snprintf(right, sizeof(right), "%u w%s %d%%", (unsigned)wordCount(), usb, bat);
  else
    snprintf(right, sizeof(right), "%u w%s", (unsigned)wordCount(), usb);
  c.setTextColor(dim);
  c.setTextDatum(top_right);
  c.drawString(right, SCREEN_W - 3, sy + 2);
  c.setTextDatum(top_left);
}
