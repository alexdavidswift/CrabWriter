#pragma once

#define APP_NAME "CrabWriter"
#define APP_VERSION "0.1.0"

// SD card layout
#define ROOT_DIR "/writer"
#define DOCS_DIR "/writer/docs"
#define FONTS_DIR "/writer/fonts"
#define SETTINGS_PATH "/writer/settings.txt"

// Cardputer / Cardputer ADV microSD wiring (SPI)
#define SD_SCK 40
#define SD_MISO 39
#define SD_MOSI 14
#define SD_CS 12

#define SCREEN_W 240
#define SCREEN_H 135
#define STATUS_H 11

// Autosave: after this much typing idle time, and at least this often while dirty.
#define AUTOSAVE_IDLE_MS 4000
#define AUTOSAVE_MAX_MS 60000

// Largest font file we will load into RAM.
#define MAX_FONT_FILE (96 * 1024)

// Keep this much heap free when growing the document buffer.
#define HEAP_RESERVE (40 * 1024)
