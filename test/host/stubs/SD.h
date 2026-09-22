// Host-side SD card: maps the card's root onto a directory on the PC.
#pragma once
#include <Arduino.h>
#include <cstdarg>
#include <memory>
#include <string>
#include <vector>

#define FILE_READ "r"
#define FILE_WRITE "w"

void hostSetSdRoot(const std::string& dir);
std::string hostSdPath(const String& p);

class File {
 public:
  File() {}
  File(FILE* f, const std::string& name, size_t size) : _f(f), _name(name), _size(size) {}
  File(const std::string& name, const std::vector<std::string>& entries, const std::string& dirPath)
      : _name(name), _isDir(true), _entries(entries), _dirPath(dirPath) {}

  explicit operator bool() const { return _f != nullptr || _isDir; }
  size_t size() const { return _size; }
  const char* name() const { return _name.c_str(); }
  bool isDirectory() const { return _isDir; }
  bool available() {
    if (!_f) return false;
    int c = fgetc(_f);
    if (c == EOF) return false;
    ungetc(c, _f);
    return true;
  }
  int read(uint8_t* buf, size_t n) { return _f ? (int)fread(buf, 1, n, _f) : -1; }
  size_t write(const uint8_t* buf, size_t n) { return _f ? fwrite(buf, 1, n, _f) : 0; }
  String readStringUntil(char term) {
    std::string s;
    int c;
    while (_f && (c = fgetc(_f)) != EOF && c != term) s += (char)c;
    return String(s);
  }
  void println(const char* s) { if (_f) fprintf(_f, "%s\n", s); }
  int printf(const char* fmt, ...) {
    if (!_f) return 0;
    va_list ap;
    va_start(ap, fmt);
    int r = vfprintf(_f, fmt, ap);
    va_end(ap);
    return r;
  }
  File openNextFile();
  void close() {
    if (_f) fclose(_f);
    _f = nullptr;
    _isDir = false;
  }

 private:
  FILE* _f = nullptr;
  std::string _name;
  size_t _size = 0;
  bool _isDir = false;
  std::vector<std::string> _entries;
  size_t _next = 0;
  std::string _dirPath;
};

class SPIClass;
class SDClass {
 public:
  bool begin(int, SPIClass&, int) { return true; }
  File open(const String& path, const char* mode = FILE_READ);
  bool exists(const String& path);
  bool remove(const String& path);
  bool rename(const String& from, const String& to);
  bool mkdir(const String& path);
};
extern SDClass SD;
