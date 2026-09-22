#pragma once
#include <Arduino.h>

// Exposes the SD card to a computer as a USB mass-storage drive.
//
// The firmware must not touch the card while the computer has it (both would
// be writing the same FAT), so drive mode is one-way: the app shows a status
// screen and restarts when you leave, which also picks up any changes made on
// the computer. Uses the ESP32-S3's single USB port, so it can't run at the
// same time as the USB keyboard host.

bool usbDriveStart();          // SD must already be mounted
bool usbDriveActive();
bool usbDriveEjected();        // the computer sent "eject"
bool usbDriveBusy();           // read/write in the last ~second
uint32_t usbDriveSizeMB();
