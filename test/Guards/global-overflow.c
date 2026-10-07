// RFC 0035 §4: a global the unit defines gets a redzone; a read past it
// traps.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 3
// RUN: not --crash %t 4 2>&1 | FileCheck %s
#include <stdlib.h>

int table[4] = {1, 2, 3, 4};

__attribute__((noinline)) static int at(const int *p, int i) {
  // CHECK: weavec: global-buffer-overflow at {{.*}}global-overflow.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  return p[i];
}

int main(int argc, char **argv) {
  return at(table, atoi(argv[argc - 1])) == 0;
}
