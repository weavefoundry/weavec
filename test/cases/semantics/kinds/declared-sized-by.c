// RFC 0030 §7.2 (WEAVEC_SIZED_BY): a synonym of WEAVEC_COUNTED_BY, counting bytes for 'void *'.
// STAGE: S6
// WEAVEC_SIZED_BY keeps its RFC 0011 meaning (elements; bytes for void *), so 'dst' is
// Sized(n) and now also produces checks: the call passing 16 bytes for an 8-byte buffer
// fails its len check before the call. The run passes n == 16.
// RUN-INPUT: 16
// ASAN
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

void fill(void *WEAVEC_SIZED_BY(n) dst, size_t n) { memset(dst, 'x', n); }

int main(int argc, char **argv) {
  char buf[8];
  size_t n = argc > 1 ? (size_t)atoi(argv[1]) : sizeof buf;
  fill(buf, n); // BUG: out-of-bounds // TRAP: len
  return buf[0] == 'x' ? 0 : 1;
}
