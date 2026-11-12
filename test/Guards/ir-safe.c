// RFC 0041 §4, §5, amendments 3 and 18; RFC 0042 §5, §6; RFC 0043 §1: a
// safe function's loop over a counted parameter compares against the
// bounds instead of reading the shadow; its public symbol validates the
// parameter once (a range guard), and a safe caller with bounds for the
// argument calls the body (external, so other safe units call it too)
// after one comparison, without the range guard. A pointer loaded from a
// field is believed: no guard. The ledger gives the replaced guards the
// reason `typed`.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -c %s -o %t.o -fweavec-ledger=%t.json
// RUN: FileCheck %s --check-prefix=LEDGER < %t.json
#include <weavec.h>
#include <stddef.h>

struct box { int *one; };

// CHECK-LABEL: define {{.*}} @sum.weavec.safe(
// CHECK-NOT: call {{.*}}@__weavec_rt_guard
// CHECK: call {{.*}}@__weavec_rt_bounds_fail
__attribute__((noinline)) WEAVEC_SAFE int
sum(const int *WEAVEC_COUNTED_BY(n) v, size_t n, size_t m) {
  int s = 0;
  for (size_t i = 0; i < m; i++)
    s += v[i];
  return s;
}

// CHECK-LABEL: define {{.*}} @deref.weavec.safe(
// CHECK-NOT: call {{.*}}@__weavec_rt_guard
// CHECK: ret
WEAVEC_SAFE int deref(struct box *b) { return *b->one; }

// CHECK-LABEL: define {{.*}} @local(
// CHECK-NOT: @__weavec_rt_range
// CHECK: call {{.*}}@sum.weavec.safe(
// CHECK-LABEL: define {{.*}} @sum(
// CHECK: call {{.*}}@__weavec_rt_range
// CHECK: @sum.weavec.safe(
WEAVEC_SAFE int local(size_t m) {
  int a[64] = {0};
  a[m & 63] = 1;
  return sum(a, 64, m);
}

// LEDGER: "reason": "typed"
