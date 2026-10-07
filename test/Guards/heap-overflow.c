// RFC 0035 §2.3: a read past a heap object traps at the access and names it.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
// RUN: %weavec_cc -O0 %s -o %t0
// RUN: not --crash %t0 2>&1 | FileCheck %s
#include <stdlib.h>

static int sum(const int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

int main(int argc, char **argv) {
  (void)argv;
  int *h = malloc(4 * sizeof *h);
  for (int i = 0; i < 4; i++)
    h[i] = i;
  // CHECK: weavec: heap-buffer-overflow at {{.*}}heap-overflow.c:11:10: read of 4 bytes at 0x{{[0-9a-f]+}}
  // CHECK: is 0 bytes after the 16-byte heap object at
  return sum(h, 4 + argc);
}
