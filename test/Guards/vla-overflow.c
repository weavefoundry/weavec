// RFC 0035 §3.4: a dynamic stack object gets a redzone after it.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 8 8
// RUN: not --crash %t 8 9 2>&1 | FileCheck %s
#include <stdlib.h>

// (Through a pointer: an index into the VLA itself is checked against its
// bound by the array-bounds checks first.)
__attribute__((noinline)) static void put(char *p, int i) {
  // CHECK: weavec: dynamic-stack-buffer-overflow at {{.*}}vla-overflow.c:[[@LINE+1]]:{{[0-9]+}}: write of 1 bytes
  p[i] = 1;
}

int main(int argc, char **argv) {
  int n = atoi(argv[argc - 2]);
  char vla[n];
  put(vla, atoi(argv[argc - 1]) - 1);
  return vla[0];
}
