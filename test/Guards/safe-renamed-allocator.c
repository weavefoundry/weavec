// RFC 0042 §6: a safe function's allocation through an allocator renamed by
// an assembler label (a test harness's `malloc`) has the size it asks for,
// and a callee without a table row does not stop the compile.
// RUN: %weavec_cc -O0 -fweavec-safe -c %s -o %t0.o
// RUN: %weavec_cc -O2 -fweavec-safe -c %s -o %t2.o
#include <weavec.h>
#include <stddef.h>

void *malloc(size_t bytes) __asm__("weavec_test_malloc");

int *WEAVEC_OWNED allocate(void) {
  int *p = malloc(4 * sizeof *p);
  if (p != NULL)
    p[3] = 1;
  return p;
}
