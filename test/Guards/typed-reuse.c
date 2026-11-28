// RFC 0045 §3, §9: in a program built from fast safe units, a block safe
// code releases is the next block of its type, at once. RFC 0050 (D3), RFC
// 0055 §2 (D2): until then the slot keeps the shadow of the object it held,
// in a program of Safe C as of Safe C++, so a stale pointer to it (here
// through guarded code) reads the zeroed object. Built with
// -fweavec-safe-checks=full, the block waits for a sweep instead, and the
// stale read stops.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s --check-prefix=FAST
// RUN: %t stale | FileCheck %s --check-prefix=STALE
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: %t.full | FileCheck %s --check-prefix=FULL
// RUN: not --crash %t.full stale 2>&1 | FileCheck %s --check-prefix=FULLREPORT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct node {
  int v;
  long w;
};

struct node *WEAVEC_OWNED make(int v) {
  struct node *n = malloc(sizeof *n);
  if (n != NULL)
    n->v = v;
  return n;
}

void drop(struct node *WEAVEC_OWNED n) { free(n); }

// (Not inlined: the optimizer may otherwise fold the comparison with a
// released block's pointer, and drop the allocation it compares.)
static const struct node *kept;
WEAVEC_GUARDED __attribute__((noinline)) void keep(const struct node *n) {
  kept = n;
}
WEAVEC_GUARDED __attribute__((noinline)) int same(const struct node *n) {
  return n == kept;
}
WEAVEC_GUARDED int peek(void) { return kept->v; }
WEAVEC_GUARDED int stale(int argc, char **argv) {
  return argc > 1 && strcmp(argv[1], "stale") == 0;
}

int main(int argc, char **argv) {
  struct node *a = make(1);
  if (a == NULL)
    return 1;
  keep(a);
  drop(a);
  if (stale(argc, argv)) {
    // STALE: stale 0
    // FULLREPORT: weavec: heap-use-after-free at {{.*}}typed-reuse.c:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
    // FULLREPORT-NOT: built for speed
    printf("stale %d\n", peek());
    return 0;
  }
  struct node *b = make(2);
  if (b == NULL)
    return 1;
  // FAST: reused
  // FULL: fresh
  printf("%s\n", same(b) ? "reused" : "fresh");
  drop(b);
  return 0;
}
