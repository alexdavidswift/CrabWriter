#pragma once
#include <cstddef>

#define MALLOC_CAP_8BIT 0

// Tests can shrink the pretend heap to exercise "memory full" handling.
extern size_t hostHeapFree;
inline size_t heap_caps_get_free_size(int) { return hostHeapFree; }
inline size_t heap_caps_get_largest_free_block(int) { return hostHeapFree; }
