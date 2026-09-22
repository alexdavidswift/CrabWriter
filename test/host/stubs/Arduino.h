// Host-side stand-in for the parts of the Arduino core the firmware uses.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

uint32_t millis();
void hostAdvanceMillis(uint32_t ms);  // tests control the clock

template <class T, class L, class H>
auto constrain(T v, L lo, H hi) -> decltype(v + lo + hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}
inline bool isDigit(char c) { return c >= '0' && c <= '9'; }

inline size_t strlcpy(char* dst, const char* src, size_t n) {
  size_t len = strlen(src);
  if (n) {
    size_t c = len < n - 1 ? len : n - 1;
    memcpy(dst, src, c);
    dst[c] = 0;
  }
  return len;
}

class String {
 public:
  String() {}
  String(const char* s) : _s(s ? s : "") {}
  String(const std::string& s) : _s(s) {}
  String(char c) : _s(1, c) {}
  String(int v) : _s(std::to_string(v)) {}
  String(unsigned v) : _s(std::to_string(v)) {}
  String(long v) : _s(std::to_string(v)) {}
  String(unsigned long v) : _s(std::to_string(v)) {}
  String(long long v) : _s(std::to_string(v)) {}
  String(unsigned long long v) : _s(std::to_string(v)) {}  // size_t on 64-bit Windows

  const char* c_str() const { return _s.c_str(); }
  size_t length() const { return _s.size(); }
  char operator[](size_t i) const { return i < _s.size() ? _s[i] : 0; }
  char& operator[](size_t i) { return _s[i]; }

  String& operator+=(const String& o) { _s += o._s; return *this; }
  String& operator+=(const char* o) { _s += o; return *this; }
  String& operator+=(char c) { _s += c; return *this; }
  friend String operator+(const String& a, const String& b) { return String(a._s + b._s); }
  friend String operator+(const String& a, const char* b) { return String(a._s + b); }
  friend String operator+(const char* a, const String& b) { return String(std::string(a) + b._s); }
  friend String operator+(const String& a, int b) { return String(a._s + std::to_string(b)); }
  friend String operator+(const String& a, unsigned b) { return String(a._s + std::to_string(b)); }
  friend String operator+(const String& a, long b) { return String(a._s + std::to_string(b)); }
  friend String operator+(const String& a, unsigned long b) { return String(a._s + std::to_string(b)); }
  friend String operator+(const String& a, char c) { return String(a._s + c); }
  bool operator==(const String& o) const { return _s == o._s; }
  bool operator==(const char* o) const { return _s == o; }
  bool operator!=(const String& o) const { return _s != o._s; }

  String substring(size_t from) const { return from >= _s.size() ? String() : String(_s.substr(from)); }
  String substring(size_t from, size_t to) const {
    if (from > to) std::swap(from, to);
    if (from >= _s.size()) return String();
    return String(_s.substr(from, to - from));
  }
  int indexOf(char c) const { auto p = _s.find(c); return p == std::string::npos ? -1 : (int)p; }
  int lastIndexOf(char c) const { auto p = _s.rfind(c); return p == std::string::npos ? -1 : (int)p; }
  bool startsWith(const String& p) const { return _s.compare(0, p._s.size(), p._s) == 0; }
  bool endsWith(const String& p) const {
    return _s.size() >= p._s.size() && _s.compare(_s.size() - p._s.size(), p._s.size(), p._s) == 0;
  }
  int compareTo(const String& o) const { return _s.compare(o._s); }
  bool equalsIgnoreCase(const String& o) const {
    if (_s.size() != o._s.size()) return false;
    for (size_t i = 0; i < _s.size(); i++)
      if (tolower((unsigned char)_s[i]) != tolower((unsigned char)o._s[i])) return false;
    return true;
  }
  long toInt() const { return strtol(_s.c_str(), nullptr, 10); }
  void trim() {
    size_t a = _s.find_first_not_of(" \t\r\n");
    size_t b = _s.find_last_not_of(" \t\r\n");
    _s = a == std::string::npos ? "" : _s.substr(a, b - a + 1);
  }
  void toLowerCase() { for (auto& c : _s) c = tolower((unsigned char)c); }
  void remove(size_t i) { if (i < _s.size()) _s.erase(i); }
  void remove(size_t i, size_t n) { if (i < _s.size()) _s.erase(i, n); }
  const std::string& str() const { return _s; }

 private:
  std::string _s;
};
