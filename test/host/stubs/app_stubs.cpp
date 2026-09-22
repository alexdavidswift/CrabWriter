// Stand-ins for the hardware-facing modules when running main.cpp on a PC:
// the input queue is fed by the test driver instead of real keyboards.
#include <M5Cardputer.h>
#include <SPI.h>
#include <deque>
#include "input.h"

M5Class M5;
HostCardputer M5Cardputer;
LGFX_Sprite hostLastFrame;
EspClass ESP;
SPIClass SPI;

static std::deque<KeyEvent> s_keys;
static uint32_t s_last = 0;

void inputBegin() {}
void inputPoll() {}
void inputPush(const KeyEvent& ev) {
  s_keys.push_back(ev);
  s_last = millis();
}
bool inputNext(KeyEvent& ev) {
  if (s_keys.empty()) return false;
  ev = s_keys.front();
  s_keys.pop_front();
  return true;
}
uint32_t inputLastActivity() { return s_last; }

bool usbKbdStart() { return true; }
