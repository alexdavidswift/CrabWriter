#pragma once
#include <Arduino.h>
#include <vector>

// Thin wrapper over the ESP32 Wi-Fi stack. Wi-Fi stays off until asked for:
// it costs ~50 KB of RAM (less room for the document) and battery.

enum NetState { NET_OFF, NET_CONNECTING, NET_CONNECTED, NET_FAILED };

struct WifiNet {
  String ssid;
  int rssi;     // dBm
  bool secure;
};

int wifiScan(std::vector<WifiNet>& out);  // blocking, ~2-4 s; returns count or -1
bool wifiConnect(const String& ssid, const String& pass);  // starts connecting
void wifiDisconnect();                    // and turns the radio off
void wifiPoll();                          // call every loop: timeouts, NTP
NetState wifiState();
String wifiIp();
int wifiSignalPercent(int rssi);
const char* wifiError();                  // why the last attempt failed
