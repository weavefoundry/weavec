// RFC 0045 §2: a program that defines its own allocator gets a fast safe
// unit's typed allocations too, as a guarded unit's `malloc` would, so
// that code the runtime does not see may release them with its `free`.
// RUN: rm -rf %t && mkdir -p %t
// RUN: %clang -O1 -c %S/Inputs/typed-program-allocator.c -o %t/alloc.o
// RUN: %weavec_cc -O2 -fweavec-safe %s %t/alloc.o -o %t/prog 2>&1 | FileCheck %s --check-prefix=NOTE
// RUN: %t/prog | FileCheck %s
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

struct node {
  int v;
  struct node *next;
};

void release(void *block);

// NOTE: defines the allocator
struct node *WEAVEC_OWNED make(int v) {
  struct node *n = calloc(1, sizeof *n);
  struct node *m = malloc(sizeof *m);
  if (n != NULL)
    n->v = v;
  free(m);
  return n;
}

// CHECK: 7
WEAVEC_GUARDED int main(void) {
  struct node *n = make(7);
  printf("%d\n", n->v);
  release(n);
  return 0;
}
