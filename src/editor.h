#pragma once
#include <M5GFX.h>
#include "gapbuffer.h"
#include "font.h"
#include "input.h"

// Soft-wrapping plain-text editor. Layout is computed lazily per paragraph
// from the view's top line, so documents of any length stay responsive.
class Editor {
 public:
  bool open(const String& fileName);  // loads DOCS_DIR/fileName (new if missing)
  bool save();
  void close();
  bool isOpen() const { return _open; }
  bool dirty() const { return _dirty; }
  const String& fileName() const { return _fileName; }
  size_t length() const { return _buf.length(); }

  void setFont(Font* f);       // not owned
  void relayout();             // call after any style change
  void handleKey(const KeyEvent& ev);
  void render(LGFX_Sprite& c, bool cursorOn);

  size_t wordCount();
  void flash(const String& msg, uint32_t ms = 1800);  // transient status message
  uint32_t lastEditMs() const { return _lastEdit; }
  uint32_t lastSaveMs() const { return _lastSave; }
  bool lastSaveFailed() const { return _saveFailed; }

 private:
  struct Line {
    size_t start, end, next;
    bool hard;  // ended by '\n'
    bool eof;   // last line of the document
  };

  Line layout(size_t start);
  size_t paraStart(size_t pos);
  size_t lineStartFor(size_t pos);
  size_t backUp(size_t lineStart, int lines);
  int charWidth(uint32_t cp);
  int xOf(size_t lineStart, size_t pos);
  size_t posAtX(size_t lineStart, int x);
  size_t lineEndPos(const Line& L);
  void ensureVisible();
  void geometry();

  void insertText(const char* s, size_t n);
  void eraseRange(size_t from, size_t to);
  size_t wordLeft(size_t pos);
  size_t wordRight(size_t pos);
  void moveVertical(int lines);

  GapBuffer _buf;
  bool _open = false;
  bool _dirty = false;
  bool _saveFailed = false;
  String _fileName;
  Font* _font = nullptr;

  size_t _cursor = 0;
  size_t _viewTop = 0;
  int _goalX = -1;

  // geometry (px)
  int _tx = 0, _ty = 0, _tw = 0, _th = 0, _lineAdv = 8, _visible = 1, _spaceW = 4;

  uint32_t _lastEdit = 0, _lastSave = 0;
  size_t _words = 0;
  bool _wordsDirty = true;
  uint32_t _wordsAt = 0;

  String _msg;
  uint32_t _msgUntil = 0;
};
