// RFC 0035 §3.2: a local whose address escapes is laid out in the frame
// with redzones; a write past it traps.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 9
// RUN: not --crash %t 11 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline)) static void fill(char *d, int n) {
  for (int i = 0; i < n; i++)
    // CHECK: weavec: stack-buffer-overflow at {{.*}}stack-overflow.c:[[@LINE+1]]:{{[0-9]+}}: write of 1 bytes
    d[i] = 'a';
}

int main(int argc, char **argv) {
  char buf[10];
  fill(buf, atoi(argv[argc - 1]));
  buf[9] = 0;
  puts(buf);
  return 0;
}
