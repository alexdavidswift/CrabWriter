#include "input.h"
#include <M5Cardputer.h>
#include <vector>

// ------------------------------------------------------------ event queue --

// A FreeRTOS queue because the USB keyboard task pushes from another core.
static QueueHandle_t s_queue = nullptr;
static volatile uint32_t s_lastActivity = 0;

void inputPush(const KeyEvent& ev) {
  if (!s_queue) return;
  xQueueSend(s_queue, &ev, 0);  // drop if full
  s_lastActivity = millis();
}

bool inputNext(KeyEvent& ev) {
  return s_queue && xQueueReceive(s_queue, &ev, 0) == pdTRUE;
}

uint32_t inputLastActivity() { return s_lastActivity; }

// ------------------------------------------------------ built-in keyboard --
//
// We read raw key positions rather than KeysState::word because the library
// applies Shift whenever Ctrl is held, and it has no key repeat.
//
// Layout (x,y): modifiers fn(0,2) shift(1,2) ctrl(0,3) opt(1,3) alt(2,3).
// Fn layer: ` = Esc, ; . , / = arrows, Backspace = Delete.
// Fn+Opt + arrows: ; = PgUp, . = PgDn, , = Home, / = End.

static const uint32_t REPEAT_DELAY_MS = 420;
static const uint32_t REPEAT_RATE_MS = 45;

struct Pos { int8_t x, y; bool operator==(const Pos& o) const { return x == o.x && y == o.y; } };

static std::vector<Pos> s_prevKeys;
static Pos s_repeatKey = {-1, -1};
static uint32_t s_repeatAt = 0;

static bool isModifier(const Pos& p) {
  return (p.y == 2 && (p.x == 0 || p.x == 1)) || (p.y == 3 && p.x <= 2);
}

static bool translate(const Pos& p, bool fn, bool shift, bool ctrl, bool opt, bool alt, KeyEvent& ev) {
  Point2D_t pt;
  pt.x = p.x;
  pt.y = p.y;
  KeyValue_t kv = M5Cardputer.Keyboard.getKeyValue(pt);
  ev = KeyEvent();
  ev.src = SRC_BUILTIN;
  ev.mods = (ctrl ? MOD_CTRL : 0) | (shift ? MOD_SHIFT : 0) | (alt ? MOD_ALT : 0);

  if (fn) {
    switch (kv.value_first) {
      case '`': ev.key = K_ESC; return true;
      case ';': ev.key = opt ? K_PGUP : K_UP; return true;
      case '.': ev.key = opt ? K_PGDN : K_DOWN; return true;
      case ',': ev.key = opt ? K_HOME : K_LEFT; return true;
      case '/': ev.key = opt ? K_END : K_RIGHT; return true;
      case KEY_BACKSPACE: ev.key = K_DELETE; return true;
      default: break;
    }
  }
  if (p.x == 13 && p.y == 0) { ev.key = K_BACKSPACE; return true; }
  if (p.x == 0 && p.y == 1) { ev.key = K_TAB; return true; }
  if (p.x == 13 && p.y == 2) { ev.key = K_ENTER; return true; }

  ev.key = K_CHAR;
  // With Ctrl/Alt held, report the unshifted key so shortcuts are stable.
  ev.ch = (shift && !ctrl && !alt) ? kv.value_second : kv.value_first;
  if (ev.ch < 32 || ev.ch > 126) return false;
  if (opt && !fn) ev.mods |= MOD_ALT;
  return true;
}

static void pollBuiltin() {
  const auto& list = M5Cardputer.Keyboard.keyList();
  bool fn = false, shift = false, ctrl = false, opt = false, alt = false;
  std::vector<Pos> keys;
  for (const auto& k : list) {
    Pos p = {(int8_t)k.x, (int8_t)k.y};
    if (p.y == 2 && p.x == 0) fn = true;
    else if (p.y == 2 && p.x == 1) shift = true;
    else if (p.y == 3 && p.x == 0) ctrl = true;
    else if (p.y == 3 && p.x == 1) opt = true;
    else if (p.y == 3 && p.x == 2) alt = true;
    if (!isModifier(p)) keys.push_back(p);
  }

  uint32_t now = millis();
  for (const Pos& p : keys) {
    bool wasDown = false;
    for (const Pos& q : s_prevKeys) if (q == p) wasDown = true;
    if (!wasDown) {
      KeyEvent ev;
      if (translate(p, fn, shift, ctrl, opt, alt, ev)) inputPush(ev);
      s_repeatKey = p;
      s_repeatAt = now + REPEAT_DELAY_MS;
    }
  }

  bool repeatHeld = false;
  for (const Pos& p : keys) if (p == s_repeatKey) repeatHeld = true;
  if (!repeatHeld) {
    s_repeatKey = {-1, -1};
  } else if ((int32_t)(now - s_repeatAt) >= 0) {
    KeyEvent ev;
    if (translate(s_repeatKey, fn, shift, ctrl, opt, alt, ev)) inputPush(ev);
    s_repeatAt = now + REPEAT_RATE_MS;
  }
  s_prevKeys = keys;
}

void inputBegin() {
  if (!s_queue) s_queue = xQueueCreate(64, sizeof(KeyEvent));
}

void inputPoll() { pollBuiltin(); }
