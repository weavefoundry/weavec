// RFC 0035 §5.3: a report names the call site of a function declared
// artificial, as the C library's fortified wrappers are, not a line inside
// it.
// RUN: %weavec_cc -O2 -g0 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// RUN: %weavec_cc -O0 %s -o %t0
// RUN: not --crash %t0 2>&1 | FileCheck %s
#include <stdlib.h>

static inline __attribute__((always_inline, artificial)) void put(char *p,
                                                                   int i) {
  p[i] = 1;
}

int main(int argc, char **argv) {
  (void)argv;
  char *h = malloc(8);
  // CHECK: weavec: heap-buffer-overflow at {{.*}}artificial-inline.c:[[@LINE+1]]:3: write of 1 bytes
  put(h, 7 + argc);
  return h[0];
}
