// RFC 0035 §7: report mode prints each failing site once and goes on.
// RUN: %weavec_cc -O2 -fweavec-checks=report %s -o %t
// RUN: %t 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) static int read(const char *p, int i) {
  // CHECK: weavec: heap-buffer-overflow at {{.*}}report-mode.c:[[@LINE+1]]:{{[0-9]+}}: read of 1 bytes
  return p[i];
}

int main(void) {
  char *p = calloc(8, 1);
  int s = 0;
  for (int round = 0; round < 3; round++)
    s += read(p, 8 + round);
  // CHECK-NEXT: weavec: 0x{{[0-9a-f]+}} is 0 bytes after the 8-byte heap object
  // CHECK-NOT: weavec:
  // CHECK: done
  printf("done %d\n", s);
  return 0;
}
