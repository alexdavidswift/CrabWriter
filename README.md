# CrabWriter

A distraction-free writing firmware for the **M5Stack Cardputer** and **Cardputer ADV**:

- **Your own pixel fonts.** Convert TTF/OTF/BDF pixel fonts (e.g. from itch.io) with
  `tools/fontconv.py`, drop them on the SD card, and flip between them while you write.
  Nine built-in fonts are always available as fallbacks.
- **Styling.** Integer text scaling (1–4×, keeps pixel fonts crisp), line and letter
  spacing, margins, 8 colour themes + a custom one, cursor style, typewriter scrolling,
  status bar, brightness. Font and style menus preview live on your actual document.
- **USB keyboard.** Plug a USB keyboard into the Cardputer's USB-C port (USB host mode).
- **Files.** Esc opens a menu to create, open, rename and delete documents. Plain `.txt`
  / `.md` files on the SD card, autosaved, with a `.bak` of the previous save.

## Building & flashing

Uses [PlatformIO](https://platformio.org/) (`pip install platformio`).

```bash
git clone https://github.com/alexdavidswift/CrabWriter.git
```

```bash
python -m platformio run
```

```bash
python -m platformio run -t upload
```

If the upload can't find the device (always the case once the USB keyboard host has been
enabled, see below), put the Cardputer into download mode: hold **G0** while switching it
on / pressing reset, then upload again.

To make a single image for M5Burner or `esptool` (flash it at offset `0x0`), merge the build
output after `python -m platformio run`:

```bash
python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 merge_bin -o dist/firmware.bin --flash_mode keep --flash_freq keep --flash_size 8MB 0x0 .pio/build/cardputer/bootloader.bin 0x8000 .pio/build/cardputer/partitions.bin 0xe000 ~/.platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin 0x10000 .pio/build/cardputer/firmware.bin
```

## SD card

Use a FAT32 microSD card. The firmware creates this layout on first boot (a template is in
[`sd/`](sd/)):

```
/writer/
  docs/          your documents (.txt / .md)
  fonts/         converted fonts (.cpf)
  settings.txt   all settings, editable by hand
```

## Fonts

No fonts are included in this repository - bring your own. Keep source fonts in `fonts/`
and converted ones in `sd/writer/fonts/`. Both are git-ignored, so fonts you don't have
the rights to share never get published by accident.

Convert fonts on your computer (needs Python + `pip install pillow`):

```bash
python tools/fontconv.py "Downloads/MyPixelFont.ttf" -o sd/writer/fonts --preview
```

- The native pixel size of TTF/OTF pixel fonts is auto-detected (the size at which the font
  renders with no anti-aliasing). If detection fails or you want a different size, pass
  `--size 16`.
- `--preview` writes a PNG next to the `.cpf` so you can check the result.
- `--name "Nice Name"` sets the name shown on the device.
- Batch: `python tools/fontconv.py fonts/*.ttf -o sd/writer/fonts`
- BDF bitmap fonts work too. Converted fonts include ASCII, Latin-1, Latin Extended-A and
  typographic punctuation (curly quotes, dashes, ellipsis) when the font has them. Missing
  glyphs fall back to ASCII look-alikes.

Copy the `.cpf` files into `/writer/fonts/` on the card. Pick one via **Esc → Fonts**
(up/down = font, left/right = size), or cycle with **Ctrl+]** / **Ctrl+[** while writing.

Please respect each font's licence. Most itch.io fonts allow personal use.

## Keys

| Action | USB keyboard | Cardputer keyboard |
|---|---|---|
| Menu | Esc | Fn + `` ` `` |
| Arrows | Arrows | Fn + `;` `.` `,` `/` |
| Page up / down | PgUp / PgDn | Fn + Opt + `;` / `.` |
| Line start / end | Home / End | Fn + Opt + `,` / `/` |
| Delete forward | Delete | Fn + Backspace |
| Word jump / delete word | Ctrl + arrows / Ctrl + Backspace | Ctrl + (same) |
| Save now | Ctrl+S | Ctrl+S |
| Next / previous font | Ctrl+] / Ctrl+[ | Ctrl+] / Ctrl+[ |
| Bigger / smaller text | Ctrl+= / Ctrl+- | Ctrl+= / Ctrl+- |
| Next theme | Ctrl+T | Ctrl+T |
| Toggle status bar | Ctrl+B | Ctrl+B |
| Document start / end | Ctrl+Home / Ctrl+End | Ctrl + Fn + Opt + `,` / `/` |

In the file list: **Enter** open, **N** new, **R** rename, **D** delete.

## USB keyboard

Enable it in **Esc → USB keyboard** (the setting is remembered, so the host starts at every
boot after that). Then connect the keyboard with a USB-C OTG adapter or a USB-C keyboard
cable.

- **Power:** on the Cardputer ADV, set the switch on the side to **5VOUT** so the port powers
  the keyboard. If a keyboard doesn't light up or isn't detected, power is the first thing
  to check. A powered hub/adapter also works.
- **The ESP32-S3 has one USB port.** While the keyboard host is running, the USB serial port
  is unavailable, so flashing needs download mode (hold G0 at power-on). Turning the option
  off takes effect after a restart.
- Supports standard HID keyboards (boot protocol), which is nearly all of them. Keyboards
  behind a USB hub, and hubs built into keyboards, are not supported by the ESP-IDF version
  used. US layout only for now. Caps Lock works, including the LED.

## Limits / known gaps

- The whole document is held in RAM (no PSRAM on the Cardputer). Expect roughly 100–150 KB of
  text per file (about 15–25k words). **Esc → Help & info** shows free RAM. For a novel, use one file per chapter.
- No undo, selection or copy/paste yet.
- Built-in fonts are ASCII-only. Use converted fonts for accented characters.

## Tests

The editor, text buffer, fonts and settings compile and run on a PC against small stand-ins
for the Arduino/SD/display libraries (`test/host/stubs/`). Needs `g++` and Python + Pillow:

```bash
bash test/host/run.sh
```

This fuzzes the gap buffer and editor (random edits and cursor moves, checking wrapping and
scrolling invariants after each), round-trips saves, feeds in corrupt font files, and writes
screenshots of the editor to `test/host/out/*.png`. The test fonts are made from Windows'
Consolas and Arial, so for now `run.sh` needs Windows.

To run the whole app on the PC and screenshot every menu screen (`test/host/out/ui/`). It
uses up to four fonts from `fonts/` if you have any, otherwise a built-in font:

```bash
bash test/host/uishots.sh
```

To preview a folder of converted fonts as contact sheets (`test/host/out/fontshots/`):

```bash
bash test/host/fontshots.sh sd/writer/fonts
``` The USB and Cardputer keyboard drivers and
the menus in `main.cpp` still need real hardware.

## Code map

| File | What it does |
|---|---|
| `src/main.cpp` | App shell: screens, menus, shortcuts, autosave |
| `src/editor.*` | Soft-wrapping editor, cursor movement, rendering, save/load |
| `src/gapbuffer.*` | UTF-8 gap buffer that grows with available heap |
| `src/font.*` | Font interface, built-in fonts, `.cpf` loader & renderer |
| `src/input.*` | Unified key events + Cardputer keyboard with key repeat |
| `src/usb_kbd.*` | USB host HID keyboard driver (own FreeRTOS task) |
| `src/settings.*` | Settings file and themes |
| `tools/fontconv.py` | TTF/OTF/BDF → `.cpf` converter (format documented in the file) |

## Licence

[MIT](LICENSE). Fonts you convert keep their own licences.
