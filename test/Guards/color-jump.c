// RFC 0037 §8: an index computed from a pointer that lands in a live
// neighbor of another color traps, where a redzone guard alone lets it
// pass; with -fno-weavec-colors it passes as before.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 0
// RUN: not --crash %t 1 2>&1 | FileCheck %s
// RUN: %weavec_cc -O2 -fno-weavec-colors %s -o %t.plain
// RUN: %t.plain 1
#include <stdint.h>
#include <stdlib.h>

static volatile long zero;

int main(int argc, char **argv) {
  int *a = malloc(64);
  int *b = malloc(64);
  const long jump = (b - a) * atol(argv[argc - 1]) + zero;
  b[0] = 0;
  // CHECK: weavec: heap-buffer-overflow at {{.*}}color-jump.c:[[@LINE+2]]:{{[0-9]+}}: write of 4 bytes
  // CHECK-NEXT: weavec: 0x{{[0-9a-f]+}} is {{[0-9]+}} bytes {{after|before}} its base 0x{{[0-9a-f]+}}, in another object
  a[jump] = 1;
  return b[0];
}
