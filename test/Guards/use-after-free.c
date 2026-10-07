// RFC 0035 §2.2: a released heap object is poisoned while it is in the
// quarantine; a read through a stale pointer traps.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>

struct node {
  int value;
  struct node *next;
};

int main(void) {
  struct node *n = malloc(sizeof *n);
  n->value = 1;
  struct node *alias = n;
  free(n);
  // CHECK: weavec: heap-use-after-free at {{.*}}use-after-free.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  printf("%d\n", alias->value);
  return 0;
}
