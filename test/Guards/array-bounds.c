// RFC 0035 §1: an index into an array whose bound the type declares is
// checked against it, inside a struct too.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 1
// RUN: not --crash %t 1 2 3 4 5 2>&1 | FileCheck %s
#include <stdio.h>

struct record {
  char name[4];
  int count;
};

int main(int argc, char **argv) {
  (void)argv;
  struct record r = {{0}, 7};
  // CHECK: weavec: index-out-of-bounds at {{.*}}array-bounds.c:[[@LINE+1]]:{{[0-9]+}}: index 5
  r.name[argc - 1] = 'x';
  printf("%d\n", r.count);
  return 0;
}
