// glibc's `_FORTIFY_SOURCE` mechanism, spelled out so every platform runs it:
// the C library's header defines `memcpy` as an inline copy that calls the
// checking builtin (Darwin's headers use macros instead).
#pragma GCC system_header
#include <string.h>
#undef memcpy
extern __inline __attribute__((__always_inline__, __gnu_inline__, __artificial__)) void *
memcpy(void *__restrict dest, const void *__restrict src, size_t len) {
  return __builtin___memcpy_chk(dest, src, len, __builtin_object_size(dest, 0));
}
