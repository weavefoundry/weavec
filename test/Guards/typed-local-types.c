// RFC 0045 §2: two C functions may each declare a `struct node`, which an
// allocation's `!alloc_token` names alike. A fast unit does not type an
// allocation of a type whose name another of its types shares, so neither
// block goes to the pool of the other; a local type no other type shares
// is typed.
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <stdlib.h>
#include <weavec.h>

[[gnu::noinline]] static void drop(void *WEAVEC_OWNED p) { free(p); }

// CHECK-LABEL: define {{.*}}@first(
// CHECK-NOT: __weavec_rt_alloc_typed
// CHECK: call {{.*}}@malloc(
void first(void) {
  struct node {
    long x;
    long y;
  };
  struct node *a = malloc(sizeof *a);
  drop(a);
}

// CHECK-LABEL: define {{.*}}@second(
// CHECK-NOT: __weavec_rt_alloc_typed
// CHECK: call {{.*}}@malloc(
void second(void) {
  struct node {
    char tag;
    double y;
  };
  struct node *b = malloc(sizeof *b);
  drop(b);
}

// CHECK-LABEL: define {{.*}}@lone(
// CHECK: call {{.*}}@__weavec_rt_alloc_typed(
void lone(void) {
  struct lone {
    long v;
  };
  struct lone *c = malloc(sizeof *c);
  drop(c);
}
