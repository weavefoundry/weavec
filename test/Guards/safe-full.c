// RFC 0041 §6, §7; RFC 0043 (O5): a use after free that ownership's rules
// cannot see (a reference loaded before a call released its block) reads
// only the released block (it is not reused while a pointer to it exists);
// with -fweavec-safe-checks=full the safe function keeps its guards and
// reports it as guarded code does. Verify mode keeps each replaced guard as
// a monitor.
// RUN: %weavec_cc -O1 -g %s -o %t
// RUN: %t
// RUN: %weavec_cc -O1 -g -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full 2>&1 | FileCheck %s
// RUN: %weavec_cc -O1 -g -fweavec-checks=verify %s -o %t.verify
// RUN: not --crash %t.verify 2>&1 | FileCheck %s --check-prefix=VERIFY
#include <weavec.h>
#include <stdlib.h>

struct box { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(n) data; size_t n; };
struct view { int *WEAVEC_COUNTED_BY(n) seen; size_t n; };

WEAVEC_SAFE __attribute__((noinline)) static void drop(struct box *b) {
  free(b->data);
  b->data = NULL;
  b->n = 0;
}

WEAVEC_SAFE __attribute__((noinline)) static int stale(struct box *b,
                                                       struct view *v) {
  int *seen = v->seen; // a reference: live when it is loaded
  drop(b);
  // CHECK: weavec: heap-use-after-free at {{.*}}safe-full.c:[[@LINE+2]]
  // VERIFY: weavec: weavec.proven: heap-use-after-free at {{.*}}safe-full.c:[[@LINE+1]]
  return seen[0];
}

int main(void) {
  struct box b = {calloc(4, sizeof(int)), 4};
  if (b.data == NULL)
    return 1;
  b.data[0] = 42;
  struct view v = {b.data, 4};
  // The released block's own bytes: 42 or what the allocator left there,
  // never another object's.
  (void)stale(&b, &v);
  return 0;
}
