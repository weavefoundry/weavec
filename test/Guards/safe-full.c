// RFC 0041 §6, §7: a use after free in a safe function reads only the
// released block (it is not reused while a pointer to it exists); with
// -fweavec-safe-checks=full the safe function keeps its guards and reports
// it as guarded code does. Verify mode keeps each replaced guard as a
// monitor.
// RUN: %weavec_cc -O1 -g %s -o %t
// RUN: %t
// RUN: %weavec_cc -O1 -g -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full 2>&1 | FileCheck %s
// RUN: %weavec_cc -O1 -g -fweavec-checks=verify %s -o %t.verify
// RUN: not --crash %t.verify 2>&1 | FileCheck %s --check-prefix=VERIFY
#include <weavec.h>
#include <stdlib.h>

WEAVEC_SAFE int stale(size_t n) {
  int *a = calloc(n, sizeof *a);
  a[0] = 42;
  free(a);
  // CHECK: weavec: heap-use-after-free at {{.*}}safe-full.c:[[@LINE+2]]
  // VERIFY: weavec: weavec.proven: heap-use-after-free at {{.*}}safe-full.c:[[@LINE+1]]
  return a[0];
}

int main(void) {
  // The released block's own bytes: 42 or what the allocator left there,
  // never another object's.
  (void)stale(4);
  return 0;
}
