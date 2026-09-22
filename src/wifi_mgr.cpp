#include "wifi_mgr.h"
#include <WiFi.h>
#include <esp_heap_caps.h>
#include <algorithm>

static NetState s_state = NET_OFF;
static uint32_t s_startedAt = 0;
static bool s_timeSet = false;
static char s_error[48] = "";

static const uint32_t CONNECT_TIMEOUT_MS = 20000;
// Wi-Fi allocates its buffers on start; refuse rather than crash if the
// document has taken most of the heap.
static const size_t WIFI_MIN_HEAP = 72 * 1024;

static bool enoughMemory() {
  if (heap_caps_get_free_size(MALLOC_CAP_8BIT) >= WIFI_MIN_HEAP) return true;
  strlcpy(s_error, "Not enough memory (document too big)", sizeof(s_error));
  return false;
}

int wifiSignalPercent(int rssi) {
  // -90 dBm is barely usable, -50 dBm is excellent.
  return constrain((rssi + 90) * 100 / 40, 0, 100);
}

int wifiScan(std::vector<WifiNet>& out) {
  out.clear();
  if (s_state == NET_OFF || s_state == NET_FAILED) {
    if (!enoughMemory()) return -1;
    WiFi.mode(WIFI_STA);
  }
  int n = WiFi.scanNetworks();
  if (n < 0) {
    strlcpy(s_error, "Scan failed", sizeof(s_error));
    if (s_state != NET_CONNECTED && s_state != NET_CONNECTING) WiFi.mode(WIFI_OFF);
    return -1;
  }
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (!ssid.length()) continue;  // hidden network
    bool dup = false;
    for (auto& w : out) {
      if (w.ssid == ssid) {  // same network on several access points: keep the strongest
        w.rssi = std::max(w.rssi, (int)WiFi.RSSI(i));
        dup = true;
      }
    }
    if (!dup) out.push_back({ssid, (int)WiFi.RSSI(i), WiFi.encryptionType(i) != WIFI_AUTH_OPEN});
  }
  WiFi.scanDelete();
  std::sort(out.begin(), out.end(), [](const WifiNet& a, const WifiNet& b) { return a.rssi > b.rssi; });
  if (s_state != NET_CONNECTED && s_state != NET_CONNECTING) WiFi.mode(WIFI_OFF);
  return out.size();
}

bool wifiConnect(const String& ssid, const String& pass) {
  s_error[0] = 0;
  if (s_state == NET_OFF || s_state == NET_FAILED) {
    if (!enoughMemory()) {
      s_state = NET_FAILED;
      return false;
    }
  }
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(ssid.c_str(), pass.length() ? pass.c_str() : nullptr);
  s_state = NET_CONNECTING;
  s_startedAt = millis();
  return true;
}

void wifiDisconnect() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  s_state = NET_OFF;
}

void wifiPoll() {
  if (s_state == NET_OFF || s_state == NET_FAILED) return;
  wl_status_t st = WiFi.status();
  if (st == WL_CONNECTED) {
    if (s_state != NET_CONNECTED) {
      s_state = NET_CONNECTED;
      // Real clock time for file timestamps (UTC; the Cardputer has no
      // battery-backed clock, so this only lasts until power off).
      if (!s_timeSet) {
        configTime(0, 0, "pool.ntp.org", "time.google.com");
        s_timeSet = true;
      }
    }
    return;
  }
  if (s_state == NET_CONNECTED) {
    // Dropped: the stack keeps retrying on its own.
    s_state = NET_CONNECTING;
    s_startedAt = millis();
    return;
  }
  bool badPass = st == WL_CONNECT_FAILED;
  bool noNet = st == WL_NO_SSID_AVAIL;
  if (badPass || noNet || millis() - s_startedAt > CONNECT_TIMEOUT_MS) {
    strlcpy(s_error, badPass ? "Wrong password?" : noNet ? "Network not found" : "Timed out",
            sizeof(s_error));
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    s_state = NET_FAILED;
  }
}

NetState wifiState() { return s_state; }

String wifiIp() { return s_state == NET_CONNECTED ? WiFi.localIP().toString() : String(""); }

const char* wifiError() { return s_error; }
