#include "usb_drive.h"
#include <SD.h>
#include "USB.h"
#include "USBMSC.h"

// Callbacks run in the TinyUSB task. Nothing else touches the card in drive
// mode, so no locking is needed.

static USBMSC s_msc;
static bool s_active = false;
static volatile bool s_ejected = false;
static volatile uint32_t s_lastIo = 0;
static uint32_t s_sectorSize = 512;
static uint8_t s_scratch[512];  // for requests that don't cover whole sectors

static int32_t onRead(uint32_t lba, uint32_t offset, void* buffer, uint32_t bufsize) {
  uint8_t* out = (uint8_t*)buffer;
  uint64_t pos = (uint64_t)lba * s_sectorSize + offset;
  uint32_t done = 0;
  while (done < bufsize) {
    uint32_t sector = pos / s_sectorSize;
    uint32_t within = pos % s_sectorSize;
    uint32_t n = min(bufsize - done, s_sectorSize - within);
    if (within == 0 && n == s_sectorSize) {
      if (!SD.readRAW(out + done, sector)) return -1;
    } else {
      if (!SD.readRAW(s_scratch, sector)) return -1;
      memcpy(out + done, s_scratch + within, n);
    }
    done += n;
    pos += n;
  }
  s_lastIo = millis();
  return bufsize;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t* buffer, uint32_t bufsize) {
  uint64_t pos = (uint64_t)lba * s_sectorSize + offset;
  uint32_t done = 0;
  while (done < bufsize) {
    uint32_t sector = pos / s_sectorSize;
    uint32_t within = pos % s_sectorSize;
    uint32_t n = min(bufsize - done, s_sectorSize - within);
    if (within == 0 && n == s_sectorSize) {
      if (!SD.writeRAW(buffer + done, sector)) return -1;
    } else {  // partial sector: read-modify-write
      if (!SD.readRAW(s_scratch, sector)) return -1;
      memcpy(s_scratch + within, buffer + done, n);
      if (!SD.writeRAW(s_scratch, sector)) return -1;
    }
    done += n;
    pos += n;
  }
  s_lastIo = millis();
  return bufsize;
}

static bool onStartStop(uint8_t, bool start, bool loadEject) {
  if (loadEject && !start) s_ejected = true;
  if (loadEject && start) s_ejected = false;
  return true;
}

bool usbDriveStart() {
  if (s_active) return true;
  size_t sectors = SD.numSectors();
  s_sectorSize = SD.sectorSize();
  if (!sectors || s_sectorSize != 512) return false;
  s_msc.vendorID("M5Stack");
  s_msc.productID("CrabWriter SD");
  s_msc.productRevision("1.0");
  s_msc.onRead(onRead);
  s_msc.onWrite(onWrite);
  s_msc.onStartStop(onStartStop);
  s_msc.mediaPresent(true);
  if (!s_msc.begin(sectors, s_sectorSize)) return false;
  USB.productName("CrabWriter");
  USB.manufacturerName("M5Stack");
  if (!USB.begin()) return false;
  s_active = true;
  return true;
}

bool usbDriveActive() { return s_active; }
bool usbDriveEjected() { return s_ejected; }
bool usbDriveBusy() { return s_active && millis() - s_lastIo < 800; }
uint32_t usbDriveSizeMB() { return (uint32_t)((uint64_t)SD.numSectors() * s_sectorSize / (1024 * 1024)); }
