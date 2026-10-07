// RFC 0035 §5.1: the allocator validates every release.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <stdlib.h>

void *(*volatile launder)(void *);
static void *same(void *p) { return p; }

int main(void) {
  launder = same;
  char *p = malloc(8);
  free(p);
  // CHECK: weavec: invalid release of 0x{{[0-9a-f]+}}: the block was already released
  free(launder(p));
  return 0;
}
