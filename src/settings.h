#pragma once
#include <Arduino.h>

// All user-tweakable options. Persisted to /writer/settings.txt as key=value
// lines, so it can also be edited by hand on a computer.
struct Settings {
  String font = "builtin:0";   // "builtin:N" or a file name in /writer/fonts
  uint8_t scale = 1;           // integer pixel scale for the text font (1-4)
  int8_t lineSpacing = 2;      // extra px between lines (can be negative)
  int8_t letterSpacing = 0;    // extra px between glyphs
  uint8_t marginX = 6;         // left/right text margin, px
  uint8_t marginY = 4;         // top/bottom text margin, px
  uint8_t theme = 0;           // index into themes[]
  uint8_t cursorStyle = 0;     // 0 bar, 1 block, 2 underline
  bool cursorBlink = true;
  bool typewriter = false;     // keep the cursor line centred
  bool statusBar = true;
  uint8_t brightness = 60;     // percent
  bool keySound = false;
  bool usbKeyboard = false;    // start the USB host at boot
  String lastFile = "";
  String wifiSsid = "";        // saved network (password stored in plain text on the card)
  String wifiPass = "";

  // Colours for the "Custom" theme, RGB888.
  uint32_t customBg = 0x1E1E2E;
  uint32_t customFg = 0xCDD6F4;
  uint32_t customAccent = 0xF5C2E7;
};

struct Theme {
  const char* name;
  uint32_t bg, fg, accent, dim;  // RGB888
};

extern Settings settings;

int themeCount();
Theme themeAt(int i);            // resolves the Custom theme from settings
uint16_t rgb565(uint32_t rgb888);

bool settingsLoad();
bool settingsSave();
