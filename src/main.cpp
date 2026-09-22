// CrabWriter - a distraction-free writing firmware for the M5Stack
// Cardputer / Cardputer ADV with custom pixel fonts and USB keyboard support.

#include <M5Cardputer.h>
#include <SD.h>
#include <SPI.h>
#include <algorithm>
#include <vector>

#include "battery.h"
#include "config.h"
#include "editor.h"
#include "font.h"
#include "input.h"
#include "settings.h"
#include "usb_drive.h"
#include "usb_kbd.h"
#include "wifi_mgr.h"

static M5Canvas canvas(&M5Cardputer.Display);
static Editor editor;
static Font* curFont = nullptr;
static std::vector<FontEntry> fontList;

enum Screen { SCR_EDITOR, SCR_MENU, SCR_FILES, SCR_FONTS, SCR_STYLE, SCR_PROMPT, SCR_CONFIRM, SCR_HELP, SCR_NOSD,
              SCR_WIFI, SCR_WIFI_SCAN, SCR_USB, SCR_DRIVE };
static Screen screen = SCR_EDITOR;
static bool needRedraw = true;

// ------------------------------------------------------------------ misc --

// Battery: sampled every 2 s and steadied by battery.cpp (voltage-only gauge).
static int s_battery = -1;  // last value drawn
static uint32_t s_batteryAt = 0;
static const uint32_t BATTERY_SAMPLE_MS = 2000;
int batteryLevel() { return batteryPercent(); }

static void applyBrightness() { M5Cardputer.Display.setBrightness(settings.brightness * 255 / 100); }

static void click() {
  if (settings.keySound) M5Cardputer.Speaker.tone(3800, 6);
}

static Theme theme() { return themeAt(settings.theme); }

static void ensureDirs() {
  if (!SD.exists(ROOT_DIR)) SD.mkdir(ROOT_DIR);
  if (!SD.exists(DOCS_DIR)) SD.mkdir(DOCS_DIR);
  if (!SD.exists(FONTS_DIR)) SD.mkdir(FONTS_DIR);
}

static bool mountSD() {
  SPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, SPI, 25000000)) return false;
  ensureDirs();
  return true;
}

// -------------------------------------------------------------- fonts --

static int fontIndexOf(const String& id) {
  for (size_t i = 0; i < fontList.size(); i++)
    if (fontList[i].id == id) return i;
  return -1;
}

// Switch the editor font; falls back to the first built-in on failure.
static bool useFont(const String& id) {
  Font* f = fontOpen(id);
  bool ok = f != nullptr;
  if (!f) f = fontOpen("builtin:0");
  editor.setFont(f);
  delete curFont;
  curFont = f;
  settings.font = ok ? id : "builtin:0";
  return ok;
}

static void cycleFont(int dir) {
  if (fontList.empty()) fontScan(fontList);
  int i = fontIndexOf(settings.font);
  int n = fontList.size();
  i = ((i < 0 ? 0 : i) + dir + n) % n;
  if (!useFont(fontList[i].id)) editor.flash("Could not load font");
  else editor.flash(fontList[i].label);
  settingsSave();
}

// ------------------------------------------------------------- documents --

struct DocEntry {
  String name;
  size_t size;
};
static std::vector<DocEntry> docs;

static bool isDocName(const String& n) {
  String l = n;
  l.toLowerCase();
  return l[0] != '.' && (l.endsWith(".txt") || l.endsWith(".md"));
}

static void scanDocs() {
  docs.clear();
  File dir = SD.open(DOCS_DIR);
  if (!dir) return;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    String n = f.name();
    int slash = n.lastIndexOf('/');
    if (slash >= 0) n = n.substring(slash + 1);
    if (!f.isDirectory() && isDocName(n)) docs.push_back({n, (size_t)f.size()});
    f.close();
  }
  dir.close();
  std::sort(docs.begin(), docs.end(), [](const DocEntry& a, const DocEntry& b) {
    return a.name.compareTo(b.name) < 0;
  });
}

static bool docExists(const String& name) { return SD.exists(String(DOCS_DIR) + "/" + name); }

static String nextDraftName() {
  for (int i = 1; i < 1000; i++) {
    String n = String("draft-") + i + ".txt";
    if (!docExists(n)) return n;
  }
  return "draft.txt";
}

static void saveIfDirty() {
  if (editor.isOpen() && editor.dirty() && !editor.save()) editor.flash("SAVE FAILED - check SD card", 4000);
}

static bool openDoc(const String& name) {
  saveIfDirty();
  String prev = editor.fileName();
  if (!editor.open(name)) {
    if (prev.length() && prev != name) editor.open(prev);  // stay where we were
    return false;
  }
  settings.lastFile = name;
  settingsSave();
  return true;
}

// Always keep a document open so the editor (and style previews) have text.
static void openSomething() {
  if (settings.lastFile.length() && docExists(settings.lastFile) && openDoc(settings.lastFile)) return;
  scanDocs();
  if (!docs.empty() && openDoc(docs[0].name)) return;
  openDoc(nextDraftName());
}

// ------------------------------------------------------------ UI helpers --

static const int UI_ROW = 16;
static const int UI_HEADER = 18;

static void uiBegin(const char* title) {
  Theme t = theme();
  canvas.fillScreen(rgb565(t.bg));
  canvas.fillRect(0, 0, SCREEN_W, UI_HEADER - 2, rgb565(t.accent));
  canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1);
  canvas.setTextDatum(top_left);
  canvas.setTextColor(rgb565(t.bg));
  canvas.drawString(title, 5, 0);
}

static void uiHint(const char* text) {
  Theme t = theme();
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  canvas.setTextColor(rgb565(t.dim));
  canvas.setTextDatum(bottom_left);
  canvas.drawString(text, 4, SCREEN_H - 1);
  canvas.setTextDatum(top_left);
}

// Draws a scrolling list. `right` may be empty or hold per-row values.
static void uiList(const std::vector<String>& items, const std::vector<String>& right, int sel, int& top) {
  Theme t = theme();
  const int rows = (SCREEN_H - UI_HEADER - 10) / UI_ROW;
  if (sel < top) top = sel;
  if (sel >= top + rows) top = sel - rows + 1;
  if (top < 0) top = 0;
  canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1);
  for (int r = 0; r < rows && top + r < (int)items.size(); r++) {
    int i = top + r;
    int y = UI_HEADER + r * UI_ROW;
    bool on = i == sel;
    if (on) canvas.fillRoundRect(2, y, SCREEN_W - 4, UI_ROW, 3, rgb565(t.fg));
    uint16_t col = on ? rgb565(t.bg) : rgb565(t.fg);
    canvas.setTextColor(col);
    canvas.setTextDatum(top_left);
    canvas.drawString(items[i], 7, y);
    if (i < (int)right.size() && right[i].length()) {
      String rv = right[i];
      int maxW = SCREEN_W - 24 - canvas.textWidth(items[i]);
      if (canvas.textWidth(rv) > maxW) {
        while (rv.length() > 1 && canvas.textWidth(rv + "..") > maxW) rv.remove(rv.length() - 1);
        rv += "..";
      }
      canvas.setTextColor(on ? rgb565(t.bg) : rgb565(t.dim));
      canvas.setTextDatum(top_right);
      canvas.drawString(rv, SCREEN_W - 7, y);
    }
  }
  // scroll indicator
  if ((int)items.size() > rows) {
    int h = SCREEN_H - UI_HEADER - 12;
    int bh = std::max(6, h * rows / (int)items.size());
    int by = UI_HEADER + (h - bh) * top / std::max(1, (int)items.size() - rows);
    canvas.fillRect(SCREEN_W - 2, by, 2, bh, rgb565(t.dim));
  }
  canvas.setTextDatum(top_left);
}

// Bottom overlay panel drawn over the live editor (fonts & style screens).
static void uiOverlay(const String& line1, const String& line2) {
  Theme t = theme();
  const int h = 30;
  int y = SCREEN_H - h;
  canvas.fillRect(0, y, SCREEN_W, h, rgb565(t.accent));
  canvas.setFont(&fonts::Font2);
  canvas.setTextSize(1);
  canvas.setTextColor(rgb565(t.bg));
  canvas.setTextDatum(top_center);
  canvas.drawString(line1, SCREEN_W / 2, y + 1);
  canvas.setFont(&fonts::Font0);
  canvas.drawString(line2, SCREEN_W / 2, y + 19);
  canvas.setTextDatum(top_left);
}

// ------------------------------------------------------------ main menu --

enum MenuItem { MI_RESUME, MI_NEW, MI_OPEN, MI_FONTS, MI_STYLE, MI_WIFI, MI_USB, MI_HELP };
static const MenuItem kMenu[] = {MI_RESUME, MI_NEW, MI_OPEN, MI_FONTS, MI_STYLE, MI_WIFI, MI_USB, MI_HELP};
static const int kMenuCount = sizeof(kMenu) / sizeof(kMenu[0]);
static int menuSel = 0, menuTop = 0;

// prompt / confirm state
enum PromptMode { PR_NEW, PR_RENAME, PR_WIFI_PASS };
static PromptMode promptMode;
static String promptTitle, promptText, promptError;
enum ConfirmMode { CF_DELETE, CF_USB, CF_DRIVE };
static ConfirmMode confirmMode;
static String confirmText;
static Screen confirmReturn;

static int filesSel = 0, filesTop = 0;
static int helpTop = 0;
static int usbSel = 0, usbTop = 0;

// Wi-Fi screens
static int wifiSel = 0, wifiTop = 0;
static std::vector<WifiNet> wifiNets;
static int wifiScanSel = 0, wifiScanTop = 0;
static int wifiScanResult = 0;
static bool wifiScanPending = false;
static String wifiPendingSsid;

static String netStateText() {
  switch (wifiState()) {
    case NET_CONNECTED: return "connected";
    case NET_CONNECTING: return "connecting...";
    case NET_FAILED: return "failed";
    default: return "off";
  }
}

static String usbModeText() {
  if (!settings.usbKeyboard) return usbKbdStarted() ? "off (restart)" : "off";
  if (!usbKbdStarted()) return "keyboard (restart)";
  return usbKbdConnected() ? "keyboard: connected" : "keyboard";
}

static void startPrompt(PromptMode m, const String& title, const String& initial) {
  promptMode = m;
  promptTitle = title;
  promptText = initial;
  promptError = "";
  screen = SCR_PROMPT;
}

static void enterDriveMode();

static void startConfirm(ConfirmMode m, const String& text, Screen ret) {
  confirmMode = m;
  confirmText = text;
  confirmReturn = ret;
  screen = SCR_CONFIRM;
}

static void drawMenu() {
  uiBegin(APP_NAME);
  std::vector<String> items, right;
  for (MenuItem m : kMenu) {
    switch (m) {
      case MI_RESUME: items.push_back("Resume writing"); right.push_back(editor.fileName()); break;
      case MI_NEW: items.push_back("New document"); right.push_back(""); break;
      case MI_OPEN: items.push_back("Open document"); right.push_back(""); break;
      case MI_FONTS: items.push_back("Fonts"); right.push_back(curFont ? curFont->name() : ""); break;
      case MI_STYLE: items.push_back("Style & theme"); right.push_back(theme().name); break;
      case MI_WIFI: items.push_back("Wi-Fi"); right.push_back(netStateText()); break;
      case MI_USB: items.push_back("USB mode"); right.push_back(usbModeText()); break;
      case MI_HELP: items.push_back("Help & info"); right.push_back(""); break;
    }
  }
  uiList(items, right, menuSel, menuTop);
  uiHint("Enter select   Esc back to writing");
}

static void menuKey(const KeyEvent& ev) {
  switch (ev.key) {
    case K_UP: menuSel = (menuSel + kMenuCount - 1) % kMenuCount; break;
    case K_DOWN: menuSel = (menuSel + 1) % kMenuCount; break;
    case K_ESC: screen = SCR_EDITOR; break;
    case K_ENTER:
      switch (kMenu[menuSel]) {
        case MI_RESUME: screen = SCR_EDITOR; break;
        case MI_NEW: startPrompt(PR_NEW, "New document", nextDraftName()); break;
        case MI_OPEN:
          scanDocs();
          filesSel = 0;
          for (size_t i = 0; i < docs.size(); i++)
            if (docs[i].name == editor.fileName()) filesSel = i;
          screen = SCR_FILES;
          break;
        case MI_FONTS: fontScan(fontList); screen = SCR_FONTS; break;
        case MI_STYLE: screen = SCR_STYLE; break;
        case MI_WIFI: wifiSel = 0; screen = SCR_WIFI; break;
        case MI_USB: usbSel = settings.usbKeyboard ? 0 : 2; screen = SCR_USB; break;
        case MI_HELP: helpTop = 0; screen = SCR_HELP; break;
      }
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- files --

static String humanSize(size_t b) {
  if (b < 1024) return String(b) + " B";
  return String((b + 512) / 1024) + " KB";
}

static void drawFiles() {
  uiBegin("Open document");
  std::vector<String> items, right;
  items.push_back("+ New document");
  right.push_back("");
  for (auto& d : docs) {
    items.push_back(d.name);
    right.push_back(humanSize(d.size));
  }
  int sel = filesSel + 1;  // row 0 is "New"
  uiList(items, right, sel, filesTop);
  uiHint("Enter open  N new  R rename  D delete");
}

static void filesKey(const KeyEvent& ev) {
  int count = docs.size() + 1;
  int sel = filesSel + 1;
  switch (ev.key) {
    case K_UP: sel = (sel + count - 1) % count; break;
    case K_DOWN: sel = (sel + 1) % count; break;
    case K_ESC: screen = SCR_MENU; break;
    case K_ENTER:
      if (sel == 0) startPrompt(PR_NEW, "New document", nextDraftName());
      else if (openDoc(docs[sel - 1].name)) screen = SCR_EDITOR;
      else editor.flash("Could not open file", 3000);
      break;
    case K_CHAR: {
      char c = tolower(ev.ch);
      if (c == 'n') startPrompt(PR_NEW, "New document", nextDraftName());
      else if (c == 'r' && sel > 0) startPrompt(PR_RENAME, "Rename", docs[sel - 1].name);
      else if (c == 'd' && sel > 0) startConfirm(CF_DELETE, "Delete " + docs[sel - 1].name + "?", SCR_FILES);
      break;
    }
    default: break;
  }
  filesSel = sel - 1;
}

// --------------------------------------------------------------- prompt --

static String cleanName(String n) {
  n.trim();
  String out;
  for (size_t i = 0; i < n.length(); i++) {
    char c = n[i];
    if (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.') out += c;
  }
  if (out.length() && out.indexOf('.') < 0) out += ".txt";
  return out;
}

static void drawPrompt() {
  Theme t = theme();
  uiBegin(promptTitle.c_str());
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(t.dim));
  canvas.drawString(promptMode == PR_WIFI_PASS ? "Password:" : "File name:", 6, 26);
  canvas.drawRect(4, 44, SCREEN_W - 8, 22, rgb565(t.fg));
  canvas.setTextColor(rgb565(t.fg));
  // Show the tail of long names.
  String shown = promptText;
  while (canvas.textWidth(shown) > SCREEN_W - 24 && shown.length()) shown.remove(0, 1);
  canvas.drawString(shown, 9, 47);
  int cx = 9 + canvas.textWidth(shown);
  canvas.fillRect(cx + 1, 47, 2, 16, rgb565(t.accent));
  if (promptError.length()) {
    canvas.setTextColor(rgb565(t.accent));
    canvas.drawString(promptError, 6, 72);
  }
  uiHint("Enter confirm   Esc cancel");
}

static void promptKey(const KeyEvent& ev) {
  switch (ev.key) {
    case K_ESC:
      screen = promptMode == PR_RENAME ? SCR_FILES : promptMode == PR_WIFI_PASS ? SCR_WIFI_SCAN : SCR_MENU;
      break;
    case K_BACKSPACE:
      if (promptText.length()) promptText.remove(promptText.length() - 1);
      break;
    case K_CHAR:
      if (!(ev.mods & MOD_CTRL) && promptText.length() < 63) promptText += (char)ev.ch;
      break;
    case K_ENTER: {
      if (promptMode == PR_WIFI_PASS) {
        if (promptText.length() < 8) { promptError = "Wi-Fi passwords are 8+ characters"; break; }
        settings.wifiSsid = wifiPendingSsid;
        settings.wifiPass = promptText;
        settingsSave();
        wifiConnect(settings.wifiSsid, settings.wifiPass);
        wifiSel = 0;
        screen = SCR_WIFI;
        break;
      }
      String name = cleanName(promptText);
      if (!name.length()) { promptError = "Please enter a name"; break; }
      if (!isDocName(name)) { promptError = "Use a .txt or .md name"; break; }
      if (docExists(name)) { promptError = "That file already exists"; break; }
      if (promptMode == PR_NEW) {
        if (openDoc(name)) {
          editor.save();  // create it on the card right away
          screen = SCR_EDITOR;
        } else {
          promptError = "Could not create file";
        }
      } else {
        String oldName = docs[filesSel].name;
        if (editor.fileName() == oldName) saveIfDirty();
        bool ok = SD.rename(String(DOCS_DIR) + "/" + oldName, String(DOCS_DIR) + "/" + name);
        if (!ok) { promptError = "Rename failed"; break; }
        SD.remove(String(DOCS_DIR) + "/" + oldName + ".bak");
        if (editor.fileName() == oldName) openDoc(name);
        scanDocs();
        for (size_t i = 0; i < docs.size(); i++) if (docs[i].name == name) filesSel = i;
        screen = SCR_FILES;
      }
      break;
    }
    default: break;
  }
}

// -------------------------------------------------------------- confirm --

static void drawConfirm() {
  Theme t = theme();
  uiBegin("Confirm");
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(t.fg));
  canvas.setTextWrap(false);
  // Simple word wrap for the question.
  String rest = confirmText, line;
  int y = 26;
  while (rest.length()) {
    int sp = rest.indexOf(' ');
    String word = sp < 0 ? rest : rest.substring(0, sp + 1);
    rest = sp < 0 ? "" : rest.substring(sp + 1);
    if (canvas.textWidth(line + word) > SCREEN_W - 12 && line.length()) {
      canvas.drawString(line, 6, y);
      y += 17;
      line = "";
    }
    line += word;
  }
  if (line.length()) canvas.drawString(line, 6, y);
  uiHint("Y yes   N / Esc no");
}

static void confirmKey(const KeyEvent& ev) {
  bool yes = ev.key == K_CHAR && tolower(ev.ch) == 'y';
  bool no = ev.key == K_ESC || (ev.key == K_CHAR && tolower(ev.ch) == 'n');
  if (!yes && !no) return;
  screen = confirmReturn;
  if (!yes) return;
  if (confirmMode == CF_DELETE) {
    String name = docs[filesSel].name;
    bool wasOpen = editor.fileName() == name;
    if (wasOpen) editor.close();
    SD.remove(String(DOCS_DIR) + "/" + name);
    SD.remove(String(DOCS_DIR) + "/" + name + ".bak");
    scanDocs();
    if (filesSel >= (int)docs.size()) filesSel = (int)docs.size() - 1;
    if (wasOpen) {
      settings.lastFile = "";
      openSomething();
    }
  } else if (confirmMode == CF_DRIVE) {
    enterDriveMode();
  } else if (confirmMode == CF_USB) {
    settings.usbKeyboard = true;
    settingsSave();
    if (usbKbdStart()) editor.flash("USB host on - plug in a keyboard", 3000);
    else editor.flash(usbKbdStatus(), 3000);
  }
}

// ------------------------------------------------------------ USB modes --

// Survives ESP.restart() (not power-off): asks the next boot to go straight
// into drive mode, before the USB keyboard host can claim the port.
static RTC_NOINIT_ATTR uint32_t s_bootIntoDrive;
static const uint32_t DRIVE_MAGIC = 0xC0FFEE42;
static bool driveEscArmed = false;

static void enterDriveMode() {
  saveIfDirty();
  settingsSave();
  if (wifiState() != NET_OFF) wifiDisconnect();
  if (usbKbdStarted()) {  // the port is taken until a restart
    s_bootIntoDrive = DRIVE_MAGIC;
    ESP.restart();
    return;
  }
  driveEscArmed = false;
  if (usbDriveStart()) screen = SCR_DRIVE;
  else editor.flash("Could not start USB drive", 3000);
}

enum UsbRow { UR_KEYBOARD, UR_DRIVE, UR_OFF, UR_COUNT };

static void drawUsb() {
  uiBegin("USB mode");
  std::vector<String> items = {"Keyboard", "Computer: SD card drive", "Off"};
  std::vector<String> right = {settings.usbKeyboard ? "on" : "", "", settings.usbKeyboard ? "" : "on"};
  uiList(items, right, usbSel, usbTop);
  const char* hint = usbSel == UR_KEYBOARD ? "USB keyboard via OTG adapter"
                   : usbSel == UR_DRIVE    ? "Edit your files on a computer"
                                           : "USB-C only charges";
  uiHint(hint);
}

static void usbKey(const KeyEvent& ev) {
  switch (ev.key) {
    case K_UP: usbSel = (usbSel + UR_COUNT - 1) % UR_COUNT; break;
    case K_DOWN: usbSel = (usbSel + 1) % UR_COUNT; break;
    case K_ESC: screen = SCR_MENU; break;
    case K_ENTER:
      if (usbSel == UR_KEYBOARD) {
        if (!settings.usbKeyboard)
          startConfirm(CF_USB, "Use a USB keyboard? The USB serial port stops working until you power off.", SCR_USB);
      } else if (usbSel == UR_DRIVE) {
        startConfirm(CF_DRIVE,
                     "Open the SD card on a computer over USB? Writing pauses until you finish, "
                     "then CrabWriter restarts. ADV: set the side switch off 5VOUT first.",
                     SCR_USB);
      } else if (settings.usbKeyboard) {
        settings.usbKeyboard = false;
        settingsSave();
        if (usbKbdStarted()) editor.flash("Keyboard stays active until restart", 3000);
      }
      break;
    default: break;
  }
}

static void drawDrive() {
  Theme t = theme();
  uiBegin("USB drive");
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(rgb565(t.fg));
  canvas.drawString("The SD card is now a drive", 6, 24);
  canvas.drawString(String("on your computer (") + usbDriveSizeMB() + " MB).", 6, 41);
  String status;
  if (usbDriveEjected()) status = "Ejected - safe to unplug.";
  else if (usbDriveBusy()) status = "Transferring...";
  else status = "Eject it there before unplugging.";
  canvas.setTextColor(rgb565(t.accent));
  canvas.drawString(status, 6, 66);
  if (driveEscArmed) {
    canvas.setTextColor(rgb565(t.fg));
    canvas.drawString("Not ejected! Esc again to restart", 6, 88);
  }
  uiHint("Esc  finish and restart");
}

static void driveKey(const KeyEvent& ev) {
  if (ev.key != K_ESC) return;
  // Restarting mid-write could corrupt the card; make that a deliberate choice.
  if (!usbDriveEjected() && !driveEscArmed) {
    driveEscArmed = true;
    return;
  }
  ESP.restart();
}

enum WifiRow { WR_NETWORK, WR_CONNECT, WR_FORGET, WR_COUNT };

static void drawWifi() {
  String title = "Wi-Fi: " + netStateText();
  uiBegin(title.c_str());
  std::vector<String> items, right;
  items.push_back("Network");
  right.push_back(settings.wifiSsid.length() ? settings.wifiSsid : String("choose..."));
  bool on = wifiState() == NET_CONNECTED || wifiState() == NET_CONNECTING;
  items.push_back(on ? "Disconnect" : "Connect");
  right.push_back("");
  items.push_back("Forget network");
  right.push_back("");
  uiList(items, right, wifiSel, wifiTop);
  String hint = "Enter select   Esc back";
  if (wifiState() == NET_CONNECTED) hint = "IP " + wifiIp();
  else if (wifiState() == NET_FAILED && wifiError()[0]) hint = wifiError();
  uiHint(hint.c_str());
}

static void startWifiScan() {
  wifiNets.clear();
  wifiScanSel = wifiScanTop = 0;
  wifiScanPending = true;  // drawn as "Scanning..." first, scanned after
  screen = SCR_WIFI_SCAN;
}

static void wifiKey(const KeyEvent& ev) {
  switch (ev.key) {
    case K_UP: wifiSel = (wifiSel + WR_COUNT - 1) % WR_COUNT; break;
    case K_DOWN: wifiSel = (wifiSel + 1) % WR_COUNT; break;
    case K_ESC: screen = SCR_MENU; break;
    case K_ENTER:
      if (wifiSel == WR_NETWORK) {
        startWifiScan();
      } else if (wifiSel == WR_CONNECT) {
        if (wifiState() == NET_CONNECTED || wifiState() == NET_CONNECTING) wifiDisconnect();
        else if (!settings.wifiSsid.length()) startWifiScan();
        else wifiConnect(settings.wifiSsid, settings.wifiPass);
      } else if (wifiSel == WR_FORGET) {
        wifiDisconnect();
        settings.wifiSsid = "";
        settings.wifiPass = "";
        settingsSave();
        editor.flash("Network forgotten");
      }
      break;
    default: break;
  }
}

static void drawWifiScan() {
  Theme t = theme();
  uiBegin("Choose network");
  if (wifiScanPending || wifiScanResult <= 0) {
    canvas.setFont(&fonts::Font2);
    canvas.setTextColor(rgb565(t.fg));
    const char* msg = wifiScanPending ? "Scanning..." : wifiScanResult < 0 ? wifiError() : "No networks found";
    canvas.drawString(msg, 6, 26);
    uiHint(wifiScanPending ? "" : "R rescan   Esc back");
    return;
  }
  std::vector<String> items, right;
  for (auto& n : wifiNets) {
    items.push_back(n.ssid);
    right.push_back(String(n.secure ? "" : "open  ") + wifiSignalPercent(n.rssi) + "%");
  }
  uiList(items, right, wifiScanSel, wifiScanTop);
  uiHint("Enter connect   R rescan   Esc back");
}

static void wifiScanKey(const KeyEvent& ev) {
  if (wifiScanPending) return;
  int n = wifiNets.size();
  switch (ev.key) {
    case K_UP: if (n) wifiScanSel = (wifiScanSel + n - 1) % n; break;
    case K_DOWN: if (n) wifiScanSel = (wifiScanSel + 1) % n; break;
    case K_ESC: screen = SCR_WIFI; break;
    case K_CHAR:
      if (tolower(ev.ch) == 'r') startWifiScan();
      break;
    case K_ENTER: {
      if (!n) break;
      const WifiNet& net = wifiNets[wifiScanSel];
      wifiPendingSsid = net.ssid;
      if (!net.secure) {
        settings.wifiSsid = net.ssid;
        settings.wifiPass = "";
        settingsSave();
        wifiConnect(net.ssid, "");
        wifiSel = 0;
        screen = SCR_WIFI;
      } else {
        startPrompt(PR_WIFI_PASS, net.ssid, net.ssid == settings.wifiSsid ? settings.wifiPass : String(""));
      }
      break;
    }
    default: break;
  }
}

// ---------------------------------------------------------------- fonts --

static void drawFonts() {
  editor.render(canvas, true);
  int i = fontIndexOf(settings.font);
  String label = i >= 0 ? fontList[i].label : String(curFont ? curFont->name() : "?");
  char l2[64];
  snprintf(l2, sizeof(l2), "%d/%d  size %dx   up/dn font  lt/rt size", i + 1, (int)fontList.size(), settings.scale);
  uiOverlay(label, l2);
}

static void fontsKey(const KeyEvent& ev) {
  int n = fontList.size();
  int i = fontIndexOf(settings.font);
  if (i < 0) i = 0;
  switch (ev.key) {
    case K_UP:
    case K_DOWN: {
      int start = i;
      // Skip fonts that fail to load, so a bad file can't trap the cursor.
      do {
        i = (i + (ev.key == K_UP ? -1 : 1) + n) % n;
      } while (!useFont(fontList[i].id) && i != start);
      break;
    }
    case K_LEFT: settings.scale = std::max(1, settings.scale - 1); editor.relayout(); break;
    case K_RIGHT: settings.scale = std::min(4, settings.scale + 1); editor.relayout(); break;
    case K_ENTER: settingsSave(); screen = SCR_MENU; break;
    case K_ESC:
      settingsSave();
      screen = SCR_MENU;
      break;
    default: break;
  }
}

// ---------------------------------------------------------------- style --

enum StyleItem { ST_SIZE, ST_LINE, ST_LETTER, ST_MARGIN_X, ST_MARGIN_Y, ST_THEME, ST_CURSOR, ST_BLINK,
                 ST_SCROLL, ST_STATUS, ST_BRIGHT, ST_SOUND, ST_COUNT };
static int styleSel = 0;

static String styleLabel(int item, String& value) {
  static const char* cursors[] = {"Bar", "Block", "Underline"};
  switch (item) {
    case ST_SIZE: value = String(settings.scale) + "x"; return "Text size";
    case ST_LINE: value = String(settings.lineSpacing) + " px"; return "Line spacing";
    case ST_LETTER: value = String(settings.letterSpacing) + " px"; return "Letter spacing";
    case ST_MARGIN_X: value = String(settings.marginX) + " px"; return "Side margins";
    case ST_MARGIN_Y: value = String(settings.marginY) + " px"; return "Top margin";
    case ST_THEME: value = theme().name; return "Theme";
    case ST_CURSOR: value = cursors[settings.cursorStyle]; return "Cursor";
    case ST_BLINK: value = settings.cursorBlink ? "On" : "Off"; return "Cursor blink";
    case ST_SCROLL: value = settings.typewriter ? "Typewriter" : "Normal"; return "Scrolling";
    case ST_STATUS: value = settings.statusBar ? "On" : "Off"; return "Status bar";
    case ST_BRIGHT: value = String(settings.brightness) + "%"; return "Brightness";
    case ST_SOUND: value = settings.keySound ? "On" : "Off"; return "Key click";
  }
  return "";
}

static void styleAdjust(int item, int d) {
  auto wrap = [](int v, int lo, int hi) { return v < lo ? hi : (v > hi ? lo : v); };
  switch (item) {
    case ST_SIZE: settings.scale = constrain(settings.scale + d, 1, 4); break;
    case ST_LINE: settings.lineSpacing = constrain(settings.lineSpacing + d, -4, 20); break;
    case ST_LETTER: settings.letterSpacing = constrain(settings.letterSpacing + d, -2, 8); break;
    case ST_MARGIN_X: settings.marginX = constrain(settings.marginX + d * 2, 0, 60); break;
    case ST_MARGIN_Y: settings.marginY = constrain(settings.marginY + d, 0, 40); break;
    case ST_THEME: settings.theme = wrap(settings.theme + d, 0, themeCount() - 1); break;
    case ST_CURSOR: settings.cursorStyle = wrap(settings.cursorStyle + d, 0, 2); break;
    case ST_BLINK: settings.cursorBlink = !settings.cursorBlink; break;
    case ST_SCROLL: settings.typewriter = !settings.typewriter; break;
    case ST_STATUS: settings.statusBar = !settings.statusBar; break;
    case ST_BRIGHT: settings.brightness = constrain(settings.brightness + d * 5, 5, 100); applyBrightness(); break;
    case ST_SOUND: settings.keySound = !settings.keySound; click(); break;
  }
  editor.relayout();
}

static void drawStyle() {
  editor.render(canvas, true);
  String value;
  String label = styleLabel(styleSel, value);
  char l2[64];
  snprintf(l2, sizeof(l2), "%d/%d   up/dn setting   lt/rt change", styleSel + 1, (int)ST_COUNT);
  uiOverlay("< " + label + ": " + value + " >", l2);
}

static void styleKey(const KeyEvent& ev) {
  switch (ev.key) {
    case K_UP: styleSel = (styleSel + ST_COUNT - 1) % ST_COUNT; break;
    case K_DOWN: styleSel = (styleSel + 1) % ST_COUNT; break;
    case K_LEFT: styleAdjust(styleSel, -1); break;
    case K_RIGHT: case K_CHAR: styleAdjust(styleSel, 1); break;
    case K_ENTER: case K_ESC: settingsSave(); screen = SCR_MENU; break;
    default: break;
  }
}

// ----------------------------------------------------------------- help --

// Font0 fits 39 characters across the screen; keep every line within that.
static const char* kHelp[] = {
  "WRITING",
  " Esc           menu (Fn+` on device)",
  " Ctrl+S        save now",
  " Ctrl+] / [    next / prev font",
  " Ctrl+= / -    bigger / smaller",
  " Ctrl+T        next theme",
  " Ctrl+B        status bar on/off",
  " Ctrl+Lt/Rt    jump word",
  " Ctrl+Bksp     delete word",
  " Ctrl+Home/End doc start / end",
  "CARDPUTER KEYS",
  " Fn + ; . , /  arrows",
  "   (in menus no Fn needed, ` = Esc)",
  " Fn+Opt + ; .  page up / down",
  " Fn+Opt + , /  line start / end",
  " Fn + Bksp     delete forward",
  "FILES",
  " Docs:  /writer/docs (.txt .md)",
  " Fonts: /writer/fonts (.cpf)",
  " Convert with tools/fontconv.py",
};
static const int kHelpCount = sizeof(kHelp) / sizeof(kHelp[0]);

static void drawHelp() {
  Theme t = theme();
  char title[48];
  snprintf(title, sizeof(title), "%s %s", APP_NAME, APP_VERSION);
  uiBegin(title);
  canvas.setFont(&fonts::Font0);
  canvas.setTextSize(1);
  const int lineH = 10;
  std::vector<String> info;
  info.push_back(String("Free RAM ") + (ESP.getFreeHeap() / 1024) + " KB   Doc " + humanSize(editor.length()));
  info.push_back(String("USB: ") + (usbKbdStarted() ? usbKbdStatus() : "off"));
  info.push_back("Wi-Fi: " + netStateText() + (wifiState() == NET_CONNECTED ? "  " + wifiIp() : String("")));
  int rows = (SCREEN_H - UI_HEADER - 12) / lineH;
  int total = kHelpCount + (int)info.size() + 1;
  helpTop = constrain(helpTop, 0, std::max(0, total - rows));
  for (int r = 0; r < rows; r++) {
    int i = helpTop + r;
    if (i >= total) break;
    String s;
    bool head = false;
    if (i < (int)info.size()) s = info[i];
    else if (i == (int)info.size()) s = "";
    else { s = kHelp[i - info.size() - 1]; head = s[0] != ' '; }
    canvas.setTextColor(head ? rgb565(t.accent) : rgb565(t.fg));
    canvas.drawString(s, 4, UI_HEADER + r * lineH);
  }
  uiHint("Up/Down scroll   Esc back");
}

static void helpKey(const KeyEvent& ev) {
  if (ev.key == K_UP) helpTop--;
  else if (ev.key == K_DOWN) helpTop++;
  else if (ev.key == K_ESC || ev.key == K_ENTER) screen = SCR_MENU;
}

// --------------------------------------------------------------- editor --

static void editorKey(const KeyEvent& ev) {
  if (ev.key == K_ESC) {
    saveIfDirty();
    menuSel = 0;
    screen = SCR_MENU;
    return;
  }
  if (ev.key == K_CHAR && (ev.mods & MOD_CTRL)) {
    switch (tolower(ev.ch)) {
      case 's': editor.flash(editor.save() ? "Saved" : "SAVE FAILED", 1500); break;
      case ']': cycleFont(1); break;
      case '[': cycleFont(-1); break;
      case '=': case '+':
        settings.scale = std::min(4, settings.scale + 1);
        editor.relayout();
        editor.flash(String("Size ") + settings.scale + "x");
        settingsSave();
        break;
      case '-':
        settings.scale = std::max(1, settings.scale - 1);
        editor.relayout();
        editor.flash(String("Size ") + settings.scale + "x");
        settingsSave();
        break;
      case 't':
        settings.theme = (settings.theme + 1) % themeCount();
        editor.flash(String("Theme: ") + theme().name);
        settingsSave();
        break;
      case 'b':
        settings.statusBar = !settings.statusBar;
        editor.relayout();
        settingsSave();
        break;
    }
    return;
  }
  editor.handleKey(ev);
}

// ------------------------------------------------------------ top level --

static void drawNoSD() {
  canvas.fillScreen(TFT_BLACK);
  canvas.setFont(&fonts::Font2);
  canvas.setTextColor(TFT_WHITE);
  canvas.setTextDatum(middle_center);
  canvas.drawString("No SD card found", SCREEN_W / 2, 50);
  canvas.setFont(&fonts::Font0);
  canvas.drawString("Insert a FAT32 microSD card", SCREEN_W / 2, 74);
  canvas.drawString("and press any key", SCREEN_W / 2, 86);
  canvas.setTextDatum(top_left);
}

static uint32_t blinkEpoch = 0;

static void draw() {
  switch (screen) {
    case SCR_EDITOR: {
      bool on = !settings.cursorBlink || ((millis() - blinkEpoch) / 530) % 2 == 0;
      editor.render(canvas, on);
      break;
    }
    case SCR_MENU: drawMenu(); break;
    case SCR_FILES: drawFiles(); break;
    case SCR_FONTS: drawFonts(); break;
    case SCR_STYLE: drawStyle(); break;
    case SCR_PROMPT: drawPrompt(); break;
    case SCR_CONFIRM: drawConfirm(); break;
    case SCR_HELP: drawHelp(); break;
    case SCR_NOSD: drawNoSD(); break;
    case SCR_WIFI: drawWifi(); break;
    case SCR_WIFI_SCAN: drawWifiScan(); break;
    case SCR_USB: drawUsb(); break;
    case SCR_DRIVE: drawDrive(); break;
  }
  canvas.pushSprite(0, 0);
}

static void startApp() {
  settingsLoad();
  applyBrightness();
  fontScan(fontList);
  if (!useFont(settings.font)) settingsSave();
  openSomething();
  screen = SCR_EDITOR;
  if (s_bootIntoDrive == DRIVE_MAGIC) {  // restarted to free the USB port
    s_bootIntoDrive = 0;
    enterDriveMode();
    return;
  }
  if (settings.usbKeyboard) usbKbdStart();
}

void setup() {
  auto cfg = M5.config();
  M5Cardputer.begin(cfg, true);
  M5Cardputer.Display.setRotation(1);
  M5Cardputer.Speaker.setVolume(80);
  canvas.setColorDepth(16);
  canvas.createSprite(SCREEN_W, SCREEN_H);
  canvas.setTextWrap(false);
  inputBegin();

  if (mountSD()) startApp();
  else screen = SCR_NOSD;
  batteryFeed(millis(), M5Cardputer.Power.getBatteryVoltage());
  s_battery = batteryPercent();
  s_batteryAt = millis();
  needRedraw = true;
}

// Screens where ; . , / ` act as arrows/Esc on the Cardputer keyboard.
static bool isNavScreen(Screen s) {
  return s != SCR_EDITOR && s != SCR_PROMPT && s != SCR_NOSD;
}

void loop() {
  M5Cardputer.update();
  inputSetNavKeys(isNavScreen(screen));
  inputPoll();

  uint32_t now = millis();
  KeyEvent ev;
  while (inputNext(ev)) {
    needRedraw = true;
    blinkEpoch = now;
    if (screen == SCR_NOSD) {
      if (mountSD()) startApp();
      continue;
    }
    if (screen == SCR_EDITOR && (ev.key == K_CHAR || ev.key == K_ENTER || ev.key == K_BACKSPACE)) click();
    switch (screen) {
      case SCR_EDITOR: editorKey(ev); break;
      case SCR_MENU: menuKey(ev); break;
      case SCR_FILES: filesKey(ev); break;
      case SCR_FONTS: fontsKey(ev); break;
      case SCR_STYLE: styleKey(ev); break;
      case SCR_PROMPT: promptKey(ev); break;
      case SCR_CONFIRM: confirmKey(ev); break;
      case SCR_HELP: helpKey(ev); break;
      case SCR_NOSD: break;
      case SCR_WIFI: wifiKey(ev); break;
      case SCR_WIFI_SCAN: wifiScanKey(ev); break;
      case SCR_USB: usbKey(ev); break;
      case SCR_DRIVE: driveKey(ev); break;
    }
  }

  // Autosave once typing pauses, and periodically during long sessions.
  if (screen != SCR_NOSD && screen != SCR_DRIVE && editor.isOpen() && editor.dirty()) {
    bool idle = now - inputLastActivity() > AUTOSAVE_IDLE_MS;
    bool overdue = now - editor.lastSaveMs() > AUTOSAVE_MAX_MS;
    bool retryWait = editor.lastSaveFailed() && now - editor.lastSaveMs() < 10000;
    if ((idle || overdue) && !retryWait) {
      if (!editor.save()) editor.flash("SAVE FAILED - check SD card", 4000);
      needRedraw = true;
    }
  }

  if (now - s_batteryAt >= BATTERY_SAMPLE_MS) {
    s_batteryAt = now;
    batteryFeed(now, M5Cardputer.Power.getBatteryVoltage());
    if (batteryPercent() != s_battery) {
      s_battery = batteryPercent();
      needRedraw = true;
    }
  }

  // Cursor blink, transient messages, and USB status changes.
  static uint32_t lastTick = 0;
  static bool lastUsb = false;
  if (now - lastTick > 265) {
    lastTick = now;
    if (screen == SCR_EDITOR || screen == SCR_MENU || screen == SCR_DRIVE || screen == SCR_WIFI ||
        screen == SCR_USB)
      needRedraw = true;
  }

  // Wi-Fi: timeouts, and tell the user when a connection attempt finishes.
  wifiPoll();
  static NetState lastNet = NET_OFF;
  if (wifiState() != lastNet) {
    NetState st = wifiState();
    if (st == NET_CONNECTED) editor.flash("Wi-Fi connected");
    else if (st == NET_FAILED) editor.flash(String("Wi-Fi: ") + wifiError(), 3000);
    lastNet = st;
    needRedraw = true;
  }
  if (usbKbdConnected() != lastUsb) {
    lastUsb = usbKbdConnected();
    editor.flash(lastUsb ? "USB keyboard connected" : "USB keyboard unplugged");
    needRedraw = true;
  }

  if (needRedraw) {
    draw();
    needRedraw = false;
  }
  // Scanning blocks for a few seconds, so do it after "Scanning..." is on screen.
  if (screen == SCR_WIFI_SCAN && wifiScanPending) {
    wifiScanResult = wifiScan(wifiNets);
    wifiScanPending = false;
    needRedraw = true;
  }
  delay(2);
}
