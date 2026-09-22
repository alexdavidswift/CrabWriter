#pragma once
#include <Arduino.h>

// Byte-oriented gap buffer holding UTF-8 text. Positions are byte offsets
// into the logical text (the gap is invisible to callers).
class GapBuffer {
 public:
  ~GapBuffer() { free(_buf); }

  void clear();
  size_t length() const { return _cap - (_gapEnd - _gapStart); }
  char at(size_t i) const { return i < _gapStart ? _buf[i] : _buf[i + (_gapEnd - _gapStart)]; }

  // Decode the codepoint starting at byte i; *len receives its byte length.
  uint32_t decode(size_t i, int* len) const;
  size_t nextChar(size_t i) const;  // start of the following codepoint
  size_t prevChar(size_t i) const;  // start of the preceding codepoint

  bool insert(size_t pos, const char* s, size_t n);
  void erase(size_t pos, size_t n);

  // Direct access for loading/saving: the two contiguous halves.
  const char* part1() const { return _buf; }
  size_t part1Len() const { return _gapStart; }
  const char* part2() const { return _buf + _gapEnd; }
  size_t part2Len() const { return _cap - _gapEnd; }

  // Make room for at least n more bytes. Fails (returns false) when the heap
  // cannot spare it - the caller shows a "document full" message.
  bool reserve(size_t n);
  // Pointer to the free space at the end, for bulk loading; call commitAppend after.
  char* appendSpace(size_t n);
  void commitAppend(size_t n);

 private:
  void moveGap(size_t pos);
  char* _buf = nullptr;
  size_t _cap = 0;
  size_t _gapStart = 0;
  size_t _gapEnd = 0;
};
