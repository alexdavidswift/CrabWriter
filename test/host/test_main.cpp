// Host-side tests for the editor, gap buffer, fonts and settings.
// Build & run with: bash test/host/run.sh

#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#define private public  // tests inspect editor internals
#include "editor.h"
#undef private
#include "config.h"
#include "font.h"
#include "gapbuffer.h"
#include "settings.h"
#include <SD.h>
#include <esp_heap_caps.h>

namespace fs = std::filesystem;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond)                                                          \
  do {                                                                       \
    g_checks++;                                                              \
    if (!(cond)) {                                                           \
      g_fail++;                                                              \
      if (g_fail < 30) printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    }                                                                        \
  } while (0)

static std::string g_root, g_out;

static std::string contents(const GapBuffer& b) {
  return std::string(b.part1(), b.part1Len()) + std::string(b.part2(), b.part2Len());
}
static std::string readFile(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}
static void writeFile(const std::string& p, const std::string& s) {
  std::ofstream f(p, std::ios::binary);
  f << s;
}
static bool isCont(char c) { return (c & 0xC0) == 0x80; }

static KeyEvent key(Key k, uint8_t mods = 0) {
  KeyEvent e;
  e.key = k;
  e.mods = mods;
  return e;
}
static KeyEvent chr(uint32_t c) {
  KeyEvent e;
  e.key = K_CHAR;
  e.ch = c;
  return e;
}
static void type(Editor& ed, const std::string& s) {
  for (unsigned char c : s) ed.handleKey(c == '\n' ? key(K_ENTER) : chr(c));
}

// ------------------------------------------------------------ gap buffer --

static void testGapBuffer() {
  printf("gap buffer fuzz\n");
  std::mt19937 rng(1234);
  GapBuffer b;
  std::string ref;
  const char* pieces[] = {"a", "hello ", "\n", "caf\xC3\xA9", "\xE2\x80\x9C", "\xE2\x80\x9D", " ", "xyz"};
  for (int step = 0; step < 20000; step++) {
    if (rng() % 3 && ref.size() < 50000) {
      std::string s = pieces[rng() % 8];
      size_t pos = ref.empty() ? 0 : rng() % (ref.size() + 1);
      CHECK(b.insert(pos, s.data(), s.size()));
      ref.insert(pos, s);
    } else if (!ref.empty()) {
      size_t pos = rng() % ref.size();
      size_t n = 1 + rng() % 8;
      b.erase(pos, n);
      ref.erase(pos, n);
    }
    if (step % 500 == 0) {
      CHECK(contents(b) == ref);
      CHECK(b.length() == ref.size());
      for (size_t i = 0; i < ref.size(); i += 37) CHECK(b.at(i) == ref[i]);
    }
  }
  // UTF-8 stepping on a clean string
  b.clear();
  std::string u = "a\xC3\xA9\xE2\x80\x9C" "b\xF0\x9F\x98\x80" "c";  // a é “ b 😀 c
  b.insert(0, u.data(), u.size());
  std::vector<size_t> fwd;
  for (size_t i = 0; i < b.length(); i = b.nextChar(i)) fwd.push_back(i);
  CHECK((fwd == std::vector<size_t>{0, 1, 3, 6, 7, 11}));
  std::vector<size_t> back;
  for (size_t i = b.length(); i > 0;) { i = b.prevChar(i); back.push_back(i); }
  CHECK((back == std::vector<size_t>{11, 7, 6, 3, 1, 0}));
  int len;
  CHECK(b.decode(1, &len) == 0xE9 && len == 2);
  CHECK(b.decode(7, &len) == 0x1F600 && len == 4);
  // Broken sequences decode as U+FFFD one byte at a time instead of hanging.
  b.clear();
  std::string bad = "\xE2\x80";
  b.insert(0, bad.data(), bad.size());
  CHECK(b.decode(0, &len) == 0xFFFD && len == 1);
  CHECK(b.nextChar(0) == 1);
}

static void testMemoryFull() {
  printf("memory limits\n");
  GapBuffer b;
  size_t saved = hostHeapFree;
  const size_t budget = HEAP_RESERVE + 8 * 1024;  // tiny pretend heap
  std::string chunk(1000, 'x');
  bool failed = false;
  for (int i = 0; i < 100 && !failed; i++) {
    hostHeapFree = budget > b._cap ? budget - b._cap : 0;  // the buffer's own memory is in use
    failed = !b.insert(b.length(), chunk.data(), chunk.size());
  }
  CHECK(failed);
  CHECK(b.length() < 10000);
  CHECK(contents(b) == std::string(b.length(), 'x'));  // nothing corrupted on failure
  hostHeapFree = saved;
}

// ------------------------------------------------------ editor invariants --

struct LineInfo { size_t start, next; bool eof; };

static std::vector<LineInfo> allLines(Editor& ed) {
  std::vector<LineInfo> out;
  size_t p = 0, n = ed._buf.length();
  for (int guard = 0; guard < 1000000; guard++) {
    Editor::Line L = ed.layout(p);
    out.push_back({L.start, L.next, L.eof});
    if (L.eof) break;
    if (L.next <= p) { CHECK(!"layout made no progress"); break; }
    p = L.next;
  }
  (void)n;
  return out;
}

static void checkInvariants(Editor& ed, const char* where) {
  int before = g_fail;
  const size_t n = ed._buf.length();
  auto lines = allLines(ed);
  CHECK(!lines.empty() && lines.front().start == 0 && lines.back().eof && lines.back().next == n);

  for (auto& li : lines) {
    Editor::Line L = ed.layout(li.start);
    CHECK(L.end <= L.next);
    if (L.hard) CHECK(ed._buf.at(L.end) == '\n' && L.next == L.end + 1);
    // Width of the visible (non-trailing-space) part must fit, unless the line
    // is a single over-wide glyph.
    size_t e = L.end;
    while (e > L.start && (ed._buf.at(e - 1) == ' ' || ed._buf.at(e - 1) == '\t')) e--;
    int w = ed.xOf(L.start, e);
    int glyphs = 0;
    for (size_t i = L.start; i < e; i = ed._buf.nextChar(i)) glyphs++;
    CHECK(w <= ed._tw || glyphs <= 1);
    CHECK(L.start == n || !isCont(ed._buf.at(L.start)));
  }

  // lineStartFor agrees with a brute-force scan for a sample of positions.
  for (size_t pos = 0; pos <= n; pos += 1 + n / 150) {
    size_t expect = 0;
    for (auto& li : lines)
      if (pos >= li.start && (pos < li.next || li.eof)) { expect = li.start; break; }
    CHECK(ed.lineStartFor(pos) == expect);
  }

  // Cursor is on a codepoint boundary and on screen.
  CHECK(ed._cursor <= n);
  CHECK(ed._cursor == n || !isCont(ed._buf.at(ed._cursor)));
  int vtIdx = -1, curIdx = -1;
  for (size_t i = 0; i < lines.size(); i++) {
    if (lines[i].start == ed._viewTop) vtIdx = i;
    if (ed._cursor >= lines[i].start && (ed._cursor < lines[i].next || lines[i].eof)) curIdx = i;
  }
  CHECK(vtIdx >= 0);  // view top is a real line start
  CHECK(curIdx >= vtIdx && curIdx < vtIdx + ed._visible);
  if (g_fail != before) printf("  (invariants failed after: %s, n=%zu cursor=%zu top=%zu)\n", where, n, ed._cursor, ed._viewTop);
}

static std::string randomText(std::mt19937& rng, size_t words) {
  static const char* w[] = {"the", "quick", "brown", "fox", "jumps", "over", "a", "lazy", "dog",
                            "extraordinarily-long-hyphenated-compound-word-that-never-ends",
                            "caf\xC3\xA9", "\xE2\x80\x9Cquoted\xE2\x80\x9D", "na\xC3\xAFve", "I",
                            "supercalifragilisticexpialidocious", "\xE2\x80\x94"};
  std::string s;
  for (size_t i = 0; i < words; i++) {
    s += w[rng() % 16];
    int r = rng() % 20;
    s += r == 0 ? "\n\n" : r == 1 ? "\n" : r == 2 ? "  " : r == 3 ? "\t" : " ";
  }
  return s;
}

static void resetStyle() {
  settings = Settings();
}

static void testEditorFuzz(Font* font, const char* label, uint32_t seed) {
  printf("editor fuzz: %s\n", label);
  std::mt19937 rng(seed);
  for (int round = 0; round < 6; round++) {
    resetStyle();
    settings.scale = 1 + rng() % 3;
    settings.lineSpacing = (int)(rng() % 8) - 2;
    settings.letterSpacing = rng() % 3;
    settings.marginX = rng() % 30;
    settings.typewriter = rng() % 3 == 0;
    settings.statusBar = rng() % 2;

    std::string name = "fuzz.txt";
    writeFile(g_root + DOCS_DIR "/" + name, randomText(rng, 50 + rng() % 400));
    Editor ed;
    CHECK(ed.open(name));
    ed.setFont(font);
    checkInvariants(ed, "open");

    std::string ref = contents(ed._buf);
    CHECK(ref.find('\r') == std::string::npos);
    for (int step = 0; step < 1500; step++) {
      size_t c = ed._cursor;
      int op = rng() % 20;
      KeyEvent ev;
      std::string expect = ref;
      if (op < 6) {
        const char* opts[] = {"a", " ", "Z", ".", "\n"};
        std::string s = opts[rng() % 5];
        ev = s == "\n" ? key(K_ENTER) : chr(s[0]);
        expect.insert(c, s);
      } else if (op == 6) {
        ev = chr(0xE9);  // é typed on a future non-US layout
        expect.insert(c, "\xC3\xA9");
      } else if (op == 7) {
        ev = key(K_BACKSPACE);
        if (c > 0) { size_t p = ed._buf.prevChar(c); expect.erase(p, c - p); }
      } else if (op == 8) {
        ev = key(K_DELETE);
        if (c < ref.size()) { size_t p = ed._buf.nextChar(c); expect.erase(c, p - c); }
      } else if (op == 9) {
        ev = key(K_BACKSPACE, MOD_CTRL);
        size_t p = ed.wordLeft(c);
        expect.erase(p, c - p);
      } else {
        static const Key moves[] = {K_LEFT, K_RIGHT, K_UP, K_DOWN, K_HOME, K_END, K_PGUP, K_PGDN};
        ev = key(moves[rng() % 8], rng() % 4 == 0 ? MOD_CTRL : 0);
      }
      ed.handleKey(ev);
      ref = contents(ed._buf);
      CHECK(ref == expect);
      if (ref != expect) break;
      if (step % 25 == 0) checkInvariants(ed, "random edit");
      // Style changes mid-session must keep everything consistent.
      if (step % 300 == 150) {
        settings.scale = 1 + rng() % 3;
        settings.marginX = rng() % 40;
        settings.typewriter = !settings.typewriter;
        ed.relayout();
        checkInvariants(ed, "relayout");
      }
    }
  }
}

static void testVerticalMovement(Font* font) {
  printf("vertical movement\n");
  resetStyle();
  writeFile(g_root + DOCS_DIR "/v.txt", "");
  Editor ed;
  ed.open("v.txt");
  ed.setFont(font);
  std::mt19937 rng(7);
  type(ed, randomText(rng, 300));
  ed.handleKey(key(K_HOME, MOD_CTRL));
  CHECK(ed._cursor == 0);
  auto lines = allLines(ed);
  auto lineOf = [&](size_t pos) {
    for (size_t i = 0; i < lines.size(); i++)
      if (pos >= lines[i].start && (pos < lines[i].next || lines[i].eof)) return (int)i;
    return -1;
  };
  // Walking down visits every line in order, then up comes back the same way.
  for (size_t i = 1; i < lines.size(); i++) {
    ed.handleKey(key(K_DOWN));
    CHECK(lineOf(ed._cursor) == (int)i);
  }
  ed.handleKey(key(K_DOWN));
  CHECK(ed._cursor == ed._buf.length());
  ed.handleKey(key(K_HOME));
  for (int i = (int)lines.size() - 2; i >= 0; i--) {
    ed.handleKey(key(K_UP));
    CHECK(lineOf(ed._cursor) == i);
  }
  // Goal column: moving through a short line keeps the original x.
  ed.close();
  writeFile(g_root + DOCS_DIR "/v.txt", "long line of text here\nab\nanother long line of text");
  ed.open("v.txt");
  ed.handleKey(key(K_HOME, MOD_CTRL));
  for (int i = 0; i < 15; i++) ed.handleKey(key(K_RIGHT));
  int x0 = ed.xOf(0, ed._cursor);
  ed.handleKey(key(K_DOWN));
  CHECK(ed._cursor == ed.lineEndPos(ed.layout(ed.lineStartFor(ed._cursor))));  // clamped to "ab"
  ed.handleKey(key(K_DOWN));
  size_t ls = ed.lineStartFor(ed._cursor);
  CHECK(std::abs(ed.xOf(ls, ed._cursor) - x0) <= font->advance('w'));
  // Word jumps
  ed.handleKey(key(K_HOME, MOD_CTRL));
  ed.handleKey(key(K_RIGHT, MOD_CTRL));
  CHECK(ed._cursor == 5);  // after "long "
  ed.handleKey(key(K_RIGHT, MOD_CTRL));
  CHECK(ed._cursor == 10);
  ed.handleKey(key(K_LEFT, MOD_CTRL));
  CHECK(ed._cursor == 5);
}

static void testEdgeCases(Font* font) {
  printf("edge cases\n");
  resetStyle();
  Editor ed;
  // Empty document
  SD.remove(DOCS_DIR "/empty.txt");
  CHECK(ed.open("empty.txt"));
  ed.setFont(font);
  checkInvariants(ed, "empty");
  for (Key k : {K_UP, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_PGUP, K_PGDN, K_BACKSPACE, K_DELETE}) {
    ed.handleKey(key(k));
    ed.handleKey(key(k, MOD_CTRL));
  }
  CHECK(ed._buf.length() == 0 && ed._cursor == 0);
  CHECK(!ed.dirty());
  // Only newlines
  type(ed, "\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n\n");
  checkInvariants(ed, "newlines");
  // One giant word wider than the screen
  type(ed, std::string(500, 'W'));
  checkInvariants(ed, "giant word");
  // Huge margins leave almost no room
  settings.marginX = 60;
  settings.scale = 4;
  ed.relayout();
  checkInvariants(ed, "tiny width");
  type(ed, " mixed words here ");
  checkInvariants(ed, "tiny width typing");
  // Huge line spacing: only one line fits
  settings.lineSpacing = 20;
  settings.marginY = 40;
  ed.relayout();
  CHECK(ed._visible >= 1);
  checkInvariants(ed, "one visible line");
  for (int i = 0; i < 30; i++) ed.handleKey(key(K_UP));
  checkInvariants(ed, "one visible line, up");
  // Ctrl+char never inserts
  size_t len = ed._buf.length();
  KeyEvent c = chr('s');
  c.mods = MOD_CTRL;
  ed.handleKey(c);
  CHECK(ed._buf.length() == len);
}

// -------------------------------------------------------------- file I/O --

static void testSaveLoad(Font* font) {
  printf("save / load\n");
  resetStyle();
  std::string path = g_root + DOCS_DIR "/io.txt";
  writeFile(path, "\xEF\xBB\xBFLine one\r\nLine two\r\n");  // BOM + CRLF, as from Windows
  Editor ed;
  CHECK(ed.open("io.txt"));
  ed.setFont(font);
  CHECK(contents(ed._buf) == "Line one\nLine two\n");
  CHECK(ed._cursor == ed._buf.length());  // resumes at the end
  CHECK(!ed.dirty());
  type(ed, "Line three");
  CHECK(ed.dirty());
  CHECK(ed.save());
  CHECK(!ed.dirty());
  CHECK(readFile(path) == "Line one\nLine two\nLine three");
  CHECK(fs::exists(path + ".bak"));
  CHECK(readFile(path + ".bak").find("Line two") != std::string::npos);
  CHECK(!fs::exists(path + ".tmp"));
  // Second save rotates the backup
  type(ed, "!");
  CHECK(ed.save());
  CHECK(readFile(path + ".bak") == "Line one\nLine two\nLine three");
  CHECK(readFile(path) == "Line one\nLine two\nLine three!");
  // A save while the gap sits mid-document still writes both halves in order
  ed.handleKey(key(K_HOME, MOD_CTRL));
  type(ed, ">> ");
  CHECK(ed.save());
  CHECK(readFile(path) == ">> Line one\nLine two\nLine three!");
  // Reopen
  Editor ed2;
  CHECK(ed2.open("io.txt"));
  ed2.setFont(font);
  CHECK(contents(ed2._buf) == ">> Line one\nLine two\nLine three!");
  // Save failure (docs dir missing) reports failure and stays dirty
  std::string moved = g_root + "/writer/docs_moved";
  fs::rename(g_root + DOCS_DIR, moved);
  type(ed2, "x");
  CHECK(!ed2.save());
  CHECK(ed2.dirty() && ed2.lastSaveFailed());
  fs::rename(moved, g_root + DOCS_DIR);
  CHECK(ed2.save() && !ed2.lastSaveFailed());
  // Too-large file is refused rather than half-loaded
  size_t saved = hostHeapFree;
  hostHeapFree = HEAP_RESERVE + 4096;
  writeFile(g_root + DOCS_DIR "/big.txt", std::string(200000, 'b'));
  Editor ed3;
  CHECK(!ed3.open("big.txt"));
  CHECK(!ed3.isOpen());
  hostHeapFree = saved;
}

static void testWordCount(Font* font) {
  printf("word count\n");
  resetStyle();
  writeFile(g_root + DOCS_DIR "/wc.txt", "  one two\tthree\n\nfour  ");
  Editor ed;
  ed.open("wc.txt");
  ed.setFont(font);
  CHECK(ed.wordCount() == 4);
  type(ed, "five");
  CHECK(ed.wordCount() == 5);
}

// ----------------------------------------------------------------- fonts --

static void testFonts() {
  printf("fonts\n");
  std::vector<FontEntry> list;
  fontScan(list);
  CHECK((int)list.size() >= builtinFontCount() + 2);
  bool sawNamed = false;
  for (auto& e : list) if (e.label == "TestTiny") sawNamed = true;  // label from the file header
  CHECK(sawNamed);

  for (auto& e : list) {
    Font* f = fontOpen(e.id);
    CHECK(f != nullptr);
    if (!f) continue;
    f->setScale(2);
    CHECK(f->lineHeight() > 0);
    CHECK(f->advance('A') > 0);
    delete f;
  }
  CHECK(fontOpen("nope.cpf") == nullptr);
  Font* b = fontOpen("builtin:999");  // out of range falls back to the first
  CHECK(b != nullptr);
  delete b;

  // Real converted font: metrics, fallback and drawing
  Font* f = fontOpen("consola-12.cpf");
  CHECK(f != nullptr);
  if (f) {
    f->setScale(1);
    int q = f->advance('?');
    CHECK(f->advance(0x4E2D) == q);   // CJK not in font -> '?'
    CHECK(f->advance(0x201C) > 0);    // curly quote present
    LGFX_Sprite s;
    s.createSprite(40, 40);
    s.fillScreen(0);
    f->draw(s, 5, 5, 'A', 0xFFFF);
    int lit = 0;
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) lit += s.pixel(x, y) != 0;
    CHECK(lit > 5);
    f->setScale(3);
    s.fillScreen(0);
    f->draw(s, 0, 0, 'A', 0xFFFF);
    int lit3 = 0;
    for (int y = 0; y < 40; y++) for (int x = 0; x < 40; x++) lit3 += s.pixel(x, y) != 0;
    CHECK(lit3 >= lit * 8);  // scale 3 -> ~9x the pixels (allowing clipping)
    delete f;
  }

  // Corrupt files are rejected, not crashed on
  std::string good = readFile(g_root + FONTS_DIR "/consola-12.cpf");
  writeFile(g_root + FONTS_DIR "/trunc.cpf", good.substr(0, good.size() / 2));
  CHECK(fontOpen("trunc.cpf") == nullptr);
  std::string bad = good;
  bad[44 + 4] = (char)0xFF;  // first glyph's bitmap offset -> way out of range
  bad[44 + 5] = (char)0xFF;
  writeFile(g_root + FONTS_DIR "/badoff.cpf", bad);
  CHECK(fontOpen("badoff.cpf") == nullptr);
  writeFile(g_root + FONTS_DIR "/junk.cpf", "not a font at all, just text padding padding padding");
  CHECK(fontOpen("junk.cpf") == nullptr);
  fs::remove(g_root + FONTS_DIR "/trunc.cpf");
  fs::remove(g_root + FONTS_DIR "/badoff.cpf");
  fs::remove(g_root + FONTS_DIR "/junk.cpf");
}

// -------------------------------------------------------------- settings --

static void testSettings() {
  printf("settings\n");
  settings = Settings();
  settings.font = "consola-12.cpf";
  settings.scale = 3;
  settings.lineSpacing = -2;
  settings.theme = themeCount() - 1;  // Custom
  settings.customBg = 0x123456;
  settings.typewriter = true;
  settings.usbKeyboard = true;
  settings.lastFile = "chapter 1.txt";
  CHECK(settingsSave());
  settings = Settings();
  CHECK(settingsLoad());
  CHECK(settings.font == "consola-12.cpf");
  CHECK(settings.scale == 3);
  CHECK(settings.lineSpacing == -2);
  CHECK(settings.theme == themeCount() - 1);
  CHECK(settings.customBg == 0x123456);
  CHECK(themeAt(settings.theme).bg == 0x123456);
  CHECK(settings.typewriter && settings.usbKeyboard);
  CHECK(settings.lastFile == "chapter 1.txt");
  // Hand-edited file with junk and out-of-range values
  writeFile(g_root + SETTINGS_PATH, "# comment\nscale=99\ntheme=amber\nbogus line\nmargin_x=-5\n\n");
  settings = Settings();
  CHECK(settingsLoad());
  CHECK(settings.scale == 4);
  CHECK(std::string(themeAt(settings.theme).name) == "Amber");
  CHECK(settings.marginX == 0);
  CHECK(rgb565(0xFFFFFF) == 0xFFFF && rgb565(0) == 0 && rgb565(0xFF0000) == 0xF800);
}

// ------------------------------------------------------------- snapshots --

static void snapshot(Font* font, int theme, int scale, int cursor, bool status, const char* file) {
  resetStyle();
  settings.theme = theme;
  settings.scale = scale;
  settings.cursorStyle = cursor;
  settings.statusBar = status;
  writeFile(g_root + DOCS_DIR "/snap.txt",
            "Chapter One\n\nThe rain had not stopped for three days. Mara pulled her coat tighter "
            "and stepped off the train, into a town that smelled of wet stone and woodsmoke.\n\n"
            "\xE2\x80\x9CYou\xE2\x80\x99re late,\xE2\x80\x9D said the man on the platform.");
  Editor ed;
  ed.open("snap.txt");
  ed.setFont(font);
  ed.handleKey(key(K_HOME, MOD_CTRL));
  for (int i = 0; i < 3; i++) ed.handleKey(key(K_DOWN));
  for (int i = 0; i < 6; i++) ed.handleKey(key(K_RIGHT));
  LGFX_Sprite s;
  s.createSprite(SCREEN_W, SCREEN_H);
  ed.render(s, true);
  CHECK(hostWritePPM(s, (g_out + "/" + file + ".ppm").c_str()));
}

int main(int argc, char** argv) {
  setvbuf(stdout, nullptr, _IONBF, 0);
  g_root = argc > 1 ? argv[1] : "test_sd";
  g_out = argc > 2 ? argv[2] : ".";
  hostSetSdRoot(g_root);
  SD.mkdir(DOCS_DIR);
  SD.mkdir(FONTS_DIR);

  testGapBuffer();
  testMemoryFull();
  testSettings();
  testFonts();

  Font* mono = fontOpen("consola-12.cpf");
  Font* prop = fontOpen("arial-11.cpf");
  Font* builtin = fontOpen("builtin:0");
  if (!mono || !prop || !builtin) {
    printf("missing test fonts\n");
    return 2;
  }
  testEditorFuzz(prop, "proportional .cpf", 11);
  testEditorFuzz(mono, "monospace .cpf", 22);
  testEditorFuzz(builtin, "built-in", 33);
  testVerticalMovement(prop);
  testEdgeCases(prop);
  testSaveLoad(prop);
  testWordCount(prop);

  snapshot(prop, 1, 1, 0, true, "snap_paper_arial");
  snapshot(mono, 0, 1, 1, true, "snap_night_consola_block");
  snapshot(mono, 2, 2, 2, false, "snap_amber_consola_2x");

  printf("\n%d checks, %d failed\n", g_checks, g_fail);
  return g_fail ? 1 : 0;
}
