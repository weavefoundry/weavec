// glibc's `_FORTIFY_SOURCE` mechanism, spelled out so every platform runs it:
// the C library's header defines `memcpy` as an inline copy that calls the
// checking builtin (Darwin's headers use macros instead). Not `__artificial__`,
// as glibc's is: a report names an artificial function's call site when it is
// inlined, and Clang does not inline this copy on arm64 Darwin.
#pragma GCC system_header
#include <string.h>
#undef memcpy
extern __inline __attribute__((__always_inline__, __gnu_inline__)) void *
memcpy(void *__restrict dest, const void *__restrict src, size_t len) {
  return __builtin___memcpy_chk(dest, src, len, __builtin_object_size(dest, 0));
}
