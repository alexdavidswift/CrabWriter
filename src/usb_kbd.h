#pragma once
#include <Arduino.h>

// USB host driver for HID boot-protocol keyboards, plugged into the
// Cardputer's USB-C port (usually through a USB-C OTG adapter).
//
// IMPORTANT: the ESP32-S3 has a single USB PHY. Once the host is started, the
// USB serial/JTAG port is gone until the next power cycle - flashing then
// needs download mode (hold G0 while powering on / pressing reset).
//
// Key events are pushed into the shared input queue from a background task.

bool usbKbdStart();         // idempotent
bool usbKbdStarted();
bool usbKbdConnected();     // a keyboard is attached and claimed
const char* usbKbdStatus(); // short human-readable state
