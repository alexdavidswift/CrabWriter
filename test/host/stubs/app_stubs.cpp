// Stand-ins for the hardware-facing modules when running main.cpp on a PC:
// the input queue is fed by the test driver instead of real keyboards.
#include <M5Cardputer.h>
#include <SPI.h>
#include <deque>
#include "input.h"
#include "usb_drive.h"
#include "wifi_mgr.h"

M5Class M5;
HostCardputer M5Cardputer;
LGFX_Sprite hostLastFrame;
EspClass ESP;
SPIClass SPI;

static std::deque<KeyEvent> s_keys;
static uint32_t s_last = 0;

void inputBegin() {}
void inputPoll() {}
void inputSetNavKeys(bool) {}
void inputPush(const KeyEvent& ev) {
  s_keys.push_back(ev);
  s_last = millis();
}
bool inputNext(KeyEvent& ev) {
  if (s_keys.empty()) return false;
  ev = s_keys.front();
  s_keys.pop_front();
  return true;
}
uint32_t inputLastActivity() { return s_last; }

bool usbKbdStart() { return true; }

// USB drive: pretend a computer has it mounted.
static bool s_drive = false;
bool usbDriveStart() { return s_drive = true; }
bool usbDriveActive() { return s_drive; }
bool usbDriveEjected() { return false; }
bool usbDriveBusy() { return false; }
uint32_t usbDriveSizeMB() { return 30436; }

// Wi-Fi: a few fake networks; connecting succeeds on the next poll.
static NetState s_net = NET_OFF;
int wifiScan(std::vector<WifiNet>& out) {
  out = {{"Harrowgate Library", -52, true}, {"Ferry Cafe Guest", -67, false}, {"TobiasPhone", -78, true}};
  return out.size();
}
bool wifiConnect(const String&, const String&) { s_net = NET_CONNECTING; return true; }
void wifiDisconnect() { s_net = NET_OFF; }
void wifiPoll() { if (s_net == NET_CONNECTING) s_net = NET_CONNECTED; }
NetState wifiState() { return s_net; }
String wifiIp() { return s_net == NET_CONNECTED ? "192.168.1.42" : ""; }
int wifiSignalPercent(int rssi) { return constrain((rssi + 90) * 100 / 40, 0, 100); }
const char* wifiError() { return ""; }
