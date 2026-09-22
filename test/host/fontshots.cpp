// Renders a sample page in every .cpf font through the real editor code.
//   fontshots <sd-root> <out-dir>   (fonts are read from <sd-root>/writer/fonts)
// Writes <out-dir>/<font>.ppm plus a manifest.txt line per font.

#include <cstdio>
#include <string>
#include <vector>

#include "config.h"
#include "editor.h"
#include "font.h"
#include "settings.h"
#include <SD.h>

static const char* kSample =
    "Chapter 3\n\n"
    "The lighthouse keeper hadn't spoken in weeks. \"Storm's coming,\" she said at last, "
    "and lit the lamp at 6:40. Nobody answered; the gulls had gone quiet too.";

int main(int argc, char** argv) {
  if (argc < 3) return 2;
  hostSetSdRoot(argv[1]);
  std::string out = argv[2];
  std::vector<FontEntry> list;
  fontScan(list);
  FILE* manifest = fopen((out + "/manifest.txt").c_str(), "w");

  {  // sample document on the fake SD card
    File f = SD.open(DOCS_DIR "/sample.txt", FILE_WRITE);
    f.write((const uint8_t*)kSample, strlen(kSample));
    f.close();
  }

  int n = 0;
  for (auto& e : list) {
    if (e.id.startsWith("builtin:")) continue;  // host stubs can't draw these
    Font* font = fontOpen(e.id);
    if (!font) {
      printf("FAILED to load %s\n", e.id.c_str());
      continue;
    }
    settings = Settings();
    settings.theme = 1;  // Paper
    settings.statusBar = false;
    settings.cursorBlink = false;
    // Show each font at the biggest integer scale that still fits several lines.
    font->setScale(1);
    int base = font->lineHeight();
    settings.scale = std::max(1, std::min(4, 22 / std::max(1, base)));
    settings.lineSpacing = settings.scale;

    Editor ed;
    ed.open("sample.txt");
    ed.setFont(font);
    KeyEvent home;
    home.key = K_HOME;
    home.mods = MOD_CTRL;
    ed.handleKey(home);  // show the top of the page so fonts are comparable
    LGFX_Sprite s;
    s.createSprite(SCREEN_W, SCREEN_H);
    ed.render(s, false);
    std::string stem = e.id.substring(0, e.id.length() - 4).str();
    hostWritePPM(s, (out + "/" + stem + ".ppm").c_str());
    fprintf(manifest, "%s\t%s\t%d\t%d\n", stem.c_str(), e.label.c_str(), base, settings.scale);
    delete font;
    n++;
  }
  fclose(manifest);
  printf("rendered %d fonts\n", n);
  return 0;
}
