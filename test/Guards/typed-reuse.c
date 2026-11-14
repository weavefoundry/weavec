// RFC 0045 §3, §9: in a program built from fast safe units, a block safe
// code releases is the next block of its type, at once; until then a stale
// pointer to it (here through guarded code) still finds it released, and
// the report says the program was built for speed. Built with
// -fweavec-safe-checks=full, the block waits for a sweep instead.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s --check-prefix=FAST
// RUN: not --crash %t stale 2>&1 | FileCheck %s
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

static const struct node *kept;
WEAVEC_GUARDED void keep(const struct node *n) { kept = n; }
WEAVEC_GUARDED int same(const struct node *n) { return n == kept; }
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
  if (stale(argc, argv))
    // CHECK: weavec: heap-use-after-free at {{.*}}typed-reuse.c:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
    // CHECK: weavec: note: built for speed; rebuild with -fweavec-safe-checks=full for allocation stacks and exact use-after-free reports
    // FULLREPORT: weavec: heap-use-after-free
    // FULLREPORT-NOT: built for speed
    return peek();
  struct node *b = make(2);
  if (b == NULL)
    return 1;
  // FAST: reused
  // FULL: fresh
  printf("%s\n", same(b) ? "reused" : "fresh");
  drop(b);
  return 0;
}
