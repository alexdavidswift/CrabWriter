// Runs the real app (src/main.cpp) on a PC, presses keys, and saves a
// screenshot of each screen.   uishots <sd-root> <out-dir>

#include <M5Cardputer.h>
#include <SD.h>
#include <string>
#include "input.h"

void setup();
void loop();

static std::string g_out;

static void press(Key k, uint32_t ch = 0, uint8_t mods = 0) {
  KeyEvent e;
  e.key = k;
  e.ch = ch;
  e.mods = mods;
  inputPush(e);
  loop();
}
static void typeText(const char* s) {
  for (; *s; s++) press(K_CHAR, (unsigned char)*s);
}
// Every capture follows a key press, which restarts the cursor blink, so the
// cursor is always visible in the frame.
static void shot(const char* name) {
  hostWritePPM(hostLastFrame, (g_out + "/" + name + ".ppm").c_str());
  printf("%s\n", name);
}

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  hostSetSdRoot(argv[1]);
  g_out = argv[2];

  setup();
  press(K_NONE);
  shot("01_editor");

  press(K_ESC);
  shot("02_menu");

  press(K_DOWN);
  press(K_DOWN);  // Open document
  press(K_ENTER);
  press(K_DOWN);
  press(K_DOWN);
  shot("03_files");

  press(K_CHAR, 'd');
  shot("04_confirm_delete");
  press(K_CHAR, 'n');  // no

  press(K_CHAR, 'n');  // new document: clear the suggested "draft-1.txt"
  for (int i = 0; i < 11; i++) press(K_BACKSPACE);
  typeText("chapter-03");
  shot("05_new_document");
  press(K_ESC);  // back to the menu, which still has "Open document" selected

  press(K_DOWN);  // Fonts
  press(K_ENTER);
  shot("06_fonts");
  press(K_DOWN);
  press(K_RIGHT);
  shot("07_fonts_next");
  press(K_UP);
  press(K_LEFT);
  press(K_ENTER);

  press(K_DOWN);  // Style & theme
  press(K_ENTER);
  shot("08_style");
  for (int i = 0; i < 5; i++) press(K_DOWN);  // Theme
  press(K_RIGHT);
  shot("09_style_theme");
  press(K_LEFT);
  press(K_ESC);

  press(K_DOWN);  // Wi-Fi
  press(K_ENTER);
  shot("10_wifi");
  press(K_ENTER);  // Network -> scan
  loop();          // the scan runs after "Scanning..." is drawn
  loop();
  shot("11_wifi_scan");
  press(K_ENTER);  // first network is secured -> password prompt
  typeText("lighthouse42");
  shot("12_wifi_password");
  press(K_ENTER);
  loop();
  shot("13_wifi_connected");
  press(K_ESC);

  press(K_DOWN);  // USB mode
  press(K_ENTER);
  shot("14_usb_mode");
  press(K_UP);  // opens on the active mode (Off); one up is "Computer: SD card drive"
  press(K_ENTER);
  shot("15_usb_drive_confirm");
  press(K_CHAR, 'y');
  shot("16_usb_drive");
  press(K_ESC);  // not ejected yet: first Esc only warns
  shot("17_usb_drive_not_ejected");
  press(K_ESC);
  if (!ESP.hostRestarted) printf("ERROR: drive mode did not restart\n");
  return 0;
}
