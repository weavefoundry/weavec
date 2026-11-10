// RFC 0041 §4, §5, §8: a safe function's loop over a counted parameter
// compares against the bounds instead of reading the shadow; the
// parameter is validated once at entry (a range guard); a pointer loaded
// from memory keeps its guard. The ledger gives the replaced guards the
// reason `safe`.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -c %s -o %t.o -fweavec-ledger=%t.json
// RUN: FileCheck %s --check-prefix=LEDGER < %t.json
#include <weavec.h>
#include <stddef.h>

struct box { int *one; };

// CHECK-LABEL: define {{.*}} @sum(
// CHECK: call {{.*}}@__weavec_rt_range
// CHECK-NOT: call {{.*}}@__weavec_rt_guard
// CHECK: call {{.*}}@__weavec_rt_bounds_fail
// CHECK-LABEL: define {{.*}} @deref(
WEAVEC_SAFE int sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n, size_t m) {
  int s = 0;
  for (size_t i = 0; i < m; i++)
    s += v[i];
  return s;
}

// CHECK: call {{.*}}@__weavec_rt_guard
WEAVEC_SAFE int deref(struct box *b) { return *b->one; }

// LEDGER: "reason": "safe"
