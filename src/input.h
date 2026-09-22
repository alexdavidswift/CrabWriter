#pragma once
#include <Arduino.h>

enum Key : uint8_t {
  K_NONE = 0,
  K_CHAR,       // printable character in .ch
  K_ENTER,
  K_BACKSPACE,
  K_DELETE,
  K_TAB,
  K_ESC,
  K_UP,
  K_DOWN,
  K_LEFT,
  K_RIGHT,
  K_HOME,
  K_END,
  K_PGUP,
  K_PGDN,
};

enum : uint8_t {
  MOD_CTRL = 1,
  MOD_SHIFT = 2,
  MOD_ALT = 4,
};

enum KeySource : uint8_t { SRC_BUILTIN, SRC_USB };

struct KeyEvent {
  Key key = K_NONE;
  uint32_t ch = 0;  // for K_CHAR (already shifted), or the base letter when Ctrl/Alt held
  uint8_t mods = 0;
  KeySource src = SRC_BUILTIN;
};

void inputBegin();
void inputPoll();                 // call every loop
bool inputNext(KeyEvent& ev);     // pop one queued event
void inputPush(const KeyEvent& ev);
uint32_t inputLastActivity();     // millis() of the last key event

// Menu screens: the Cardputer's ; . , / keys act as arrows and ` as Esc
// without holding Fn (their printed characters are useless in a menu).
void inputSetNavKeys(bool on);
