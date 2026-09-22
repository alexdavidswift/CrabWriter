// Host-side stand-in for the M5Cardputer library: enough for main.cpp to run
// on a PC. Frames pushed to the "display" are kept in hostLastFrame.
#pragma once
#include <Arduino.h>
#include <M5GFX.h>

struct M5Config {};
struct M5Class {
  M5Config config() { return {}; }
};
extern M5Class M5;

struct HostDisplay {
  void setRotation(int) {}
  void setBrightness(int) {}
};
struct HostSpeaker {
  void setVolume(int) {}
  void tone(int, int) {}
};
struct HostPower {
  int getBatteryLevel() { return 87; }
};
struct HostCardputer {
  HostDisplay Display;
  HostSpeaker Speaker;
  HostPower Power;
  void begin(M5Config, bool) {}
  void update() {}
};
extern HostCardputer M5Cardputer;

extern LGFX_Sprite hostLastFrame;

class M5Canvas : public LGFX_Sprite {
 public:
  explicit M5Canvas(HostDisplay*) {}
  void pushSprite(int, int) { hostLastFrame = *this; }
};

struct EspClass {
  uint32_t getFreeHeap() { return 182 * 1024; }
  void restart() { hostRestarted = true; }
  bool hostRestarted = false;
};
extern EspClass ESP;

inline void delay(uint32_t ms) { hostAdvanceMillis(ms); }
