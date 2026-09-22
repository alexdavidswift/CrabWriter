#include "settings.h"
#include "config.h"
#include <SD.h>

Settings settings;

static const Theme kThemes[] = {
  {"Night",      0x101010, 0xD8D8D0, 0x7FB4FF, 0x606060},
  {"Paper",      0xF2EAD8, 0x2A2622, 0xB0452E, 0x9A9080},
  {"Amber",      0x0A0600, 0xFFB000, 0xFFD27A, 0x7A5400},
  {"Phosphor",   0x020C04, 0x33FF66, 0xB0FFC4, 0x157A30},
  {"Ink",        0x0B1D3A, 0xE8ECF4, 0xFFC857, 0x5A6A88},
  {"Solar Dark", 0x002B36, 0x93A1A1, 0xB58900, 0x586E75},
  {"Solar Light",0xFDF6E3, 0x586E75, 0xCB4B16, 0x93A1A1},
  {"Pocket",     0x9BBC0F, 0x0F380F, 0x306230, 0x6A8F1F},
  {"Custom",     0, 0, 0, 0},
};
static const int kThemeCount = sizeof(kThemes) / sizeof(kThemes[0]);

int themeCount() { return kThemeCount; }

static uint32_t mix(uint32_t a, uint32_t b) {  // 50/50 blend
  return (((a >> 16 & 0xFF) + (b >> 16 & 0xFF)) / 2) << 16 |
         (((a >> 8 & 0xFF) + (b >> 8 & 0xFF)) / 2) << 8 |
         (((a & 0xFF) + (b & 0xFF)) / 2);
}

Theme themeAt(int i) {
  if (i < 0 || i >= kThemeCount) i = 0;
  Theme t = kThemes[i];
  if (i == kThemeCount - 1) {
    t.bg = settings.customBg;
    t.fg = settings.customFg;
    t.accent = settings.customAccent;
    t.dim = mix(settings.customBg, settings.customFg);
  }
  return t;
}

uint16_t rgb565(uint32_t c) {
  return ((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F);
}

static bool parseBool(const String& v) { return v == "1" || v == "true" || v == "on" || v == "yes"; }

bool settingsLoad() {
  File f = SD.open(SETTINGS_PATH, FILE_READ);
  if (!f) return false;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    line.trim();
    if (line.length() == 0 || line[0] == '#') continue;
    int eq = line.indexOf('=');
    if (eq < 0) continue;
    String k = line.substring(0, eq);
    String v = line.substring(eq + 1);
    k.trim();
    v.trim();
    if (k == "font") settings.font = v;
    else if (k == "scale") settings.scale = constrain(v.toInt(), 1, 4);
    else if (k == "line_spacing") settings.lineSpacing = constrain(v.toInt(), -4, 20);
    else if (k == "letter_spacing") settings.letterSpacing = constrain(v.toInt(), -2, 8);
    else if (k == "margin_x") settings.marginX = constrain(v.toInt(), 0, 60);
    else if (k == "margin_y") settings.marginY = constrain(v.toInt(), 0, 40);
    else if (k == "theme") {
      // Accept either an index or a theme name.
      settings.theme = 0;
      for (int i = 0; i < kThemeCount; i++)
        if (v.equalsIgnoreCase(kThemes[i].name)) settings.theme = i;
      if (isDigit(v[0])) settings.theme = constrain(v.toInt(), 0, kThemeCount - 1);
    }
    else if (k == "cursor") settings.cursorStyle = constrain(v.toInt(), 0, 2);
    else if (k == "cursor_blink") settings.cursorBlink = parseBool(v);
    else if (k == "typewriter") settings.typewriter = parseBool(v);
    else if (k == "status_bar") settings.statusBar = parseBool(v);
    else if (k == "brightness") settings.brightness = constrain(v.toInt(), 5, 100);
    else if (k == "key_sound") settings.keySound = parseBool(v);
    else if (k == "usb_keyboard") settings.usbKeyboard = parseBool(v);
    else if (k == "last_file") settings.lastFile = v;
    else if (k == "wifi_ssid") settings.wifiSsid = v;
    else if (k == "wifi_password") settings.wifiPass = v;
    else if (k == "custom_bg") settings.customBg = strtoul(v.c_str(), nullptr, 16);
    else if (k == "custom_fg") settings.customFg = strtoul(v.c_str(), nullptr, 16);
    else if (k == "custom_accent") settings.customAccent = strtoul(v.c_str(), nullptr, 16);
  }
  f.close();
  return true;
}

bool settingsSave() {
  File f = SD.open(SETTINGS_PATH, FILE_WRITE);
  if (!f) return false;
  f.println("# CrabWriter settings. Safe to edit on a computer.");
  f.printf("font=%s\n", settings.font.c_str());
  f.printf("scale=%d\n", settings.scale);
  f.printf("line_spacing=%d\n", settings.lineSpacing);
  f.printf("letter_spacing=%d\n", settings.letterSpacing);
  f.printf("margin_x=%d\n", settings.marginX);
  f.printf("margin_y=%d\n", settings.marginY);
  f.printf("theme=%s\n", kThemes[settings.theme].name);
  f.printf("cursor=%d\n", settings.cursorStyle);
  f.printf("cursor_blink=%d\n", settings.cursorBlink);
  f.printf("typewriter=%d\n", settings.typewriter);
  f.printf("status_bar=%d\n", settings.statusBar);
  f.printf("brightness=%d\n", settings.brightness);
  f.printf("key_sound=%d\n", settings.keySound);
  f.printf("usb_keyboard=%d\n", settings.usbKeyboard);
  f.printf("last_file=%s\n", settings.lastFile.c_str());
  f.printf("wifi_ssid=%s\n", settings.wifiSsid.c_str());
  f.printf("wifi_password=%s\n", settings.wifiPass.c_str());
  f.println("# Colours for the Custom theme (hex RRGGBB)");
  f.printf("custom_bg=%06lX\n", (unsigned long)settings.customBg);
  f.printf("custom_fg=%06lX\n", (unsigned long)settings.customFg);
  f.printf("custom_accent=%06lX\n", (unsigned long)settings.customAccent);
  f.close();
  return true;
}
