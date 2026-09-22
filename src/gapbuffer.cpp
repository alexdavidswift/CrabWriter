#include "gapbuffer.h"
#include "config.h"
#include <esp_heap_caps.h>

void GapBuffer::clear() {
  _gapStart = 0;
  _gapEnd = _cap;
}

uint32_t GapBuffer::decode(size_t i, int* len) const {
  size_t n = length();
  uint8_t c = at(i);
  int need;
  uint32_t cp;
  if (c < 0x80) { *len = 1; return c; }
  else if ((c & 0xE0) == 0xC0) { need = 1; cp = c & 0x1F; }
  else if ((c & 0xF0) == 0xE0) { need = 2; cp = c & 0x0F; }
  else if ((c & 0xF8) == 0xF0) { need = 3; cp = c & 0x07; }
  else { *len = 1; return 0xFFFD; }  // stray continuation byte
  if (i + need >= n) { *len = 1; return 0xFFFD; }  // truncated sequence
  for (int k = 1; k <= need; k++) {
    uint8_t cc = at(i + k);
    if ((cc & 0xC0) != 0x80) { *len = 1; return 0xFFFD; }
    cp = (cp << 6) | (cc & 0x3F);
  }
  *len = need + 1;
  return cp;
}

size_t GapBuffer::nextChar(size_t i) const {
  size_t n = length();
  if (i >= n) return n;
  int len;
  decode(i, &len);
  return i + len;
}

size_t GapBuffer::prevChar(size_t i) const {
  if (i == 0) return 0;
  size_t j = i - 1;
  // Step back over up to 3 continuation bytes.
  for (int k = 0; k < 3 && j > 0 && (at(j) & 0xC0) == 0x80; k++) j--;
  int len;
  decode(j, &len);
  return (j + len == i) ? j : i - 1;
}

void GapBuffer::moveGap(size_t pos) {
  if (pos == _gapStart) return;
  size_t gap = _gapEnd - _gapStart;
  if (pos < _gapStart) {
    size_t n = _gapStart - pos;
    memmove(_buf + pos + gap, _buf + pos, n);
  } else {
    size_t n = pos - _gapStart;
    memmove(_buf + _gapStart, _buf + _gapEnd, n);
  }
  _gapStart = pos;
  _gapEnd = pos + gap;
}

bool GapBuffer::reserve(size_t n) {
  size_t gap = _gapEnd - _gapStart;
  if (gap >= n) return true;
  size_t len = length();
  // Grow generously so typing does not realloc constantly, but never eat into
  // the heap reserve the rest of the app (USB, fonts, SD) needs.
  size_t want = len + n + 4096 + len / 8;
  auto fits = [&](size_t w) {
    size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);
    size_t freeHeap = heap_caps_get_free_size(MALLOC_CAP_8BIT);
    return largest >= w && freeHeap >= (w - _cap) + HEAP_RESERVE;
  };
  if (!fits(want)) {
    want = len + n + 256;  // fall back to the bare minimum
    if (!fits(want)) return false;
  }
  char* nb = (char*)realloc(_buf, want);
  if (!nb) return false;
  // Move the tail (after the gap) to the end of the new allocation.
  size_t tail = _cap - _gapEnd;
  memmove(nb + want - tail, nb + _gapEnd, tail);
  _buf = nb;
  _gapEnd = want - tail;
  _cap = want;
  return true;
}

bool GapBuffer::insert(size_t pos, const char* s, size_t n) {
  if (pos > length()) pos = length();
  if (!reserve(n)) return false;
  moveGap(pos);
  memcpy(_buf + _gapStart, s, n);
  _gapStart += n;
  return true;
}

void GapBuffer::erase(size_t pos, size_t n) {
  size_t len = length();
  if (pos >= len) return;
  if (pos + n > len) n = len - pos;
  moveGap(pos);
  _gapEnd += n;
}

char* GapBuffer::appendSpace(size_t n) {
  if (!reserve(n)) return nullptr;
  moveGap(length());
  return _buf + _gapStart;
}

void GapBuffer::commitAppend(size_t n) { _gapStart += n; }
