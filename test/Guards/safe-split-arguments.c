// RFC 0043 amendment 17: where the ABI splits a parameter (a 16-byte struct
// in two registers on x86-64), no argument is paired with a bound: a result
// counted by a parameter keeps its guards instead of empty bounds, and the
// function is not split into an entry and a body.
// RUN: %weavec_cc --target=x86_64-linux-gnu -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <weavec.h>

struct pair { long a, b; };
typedef unsigned long size_t;

int *WEAVEC_COUNTED_BY(n) make(struct pair p, size_t n);

// CHECK-LABEL: define {{.*}} @first(
// CHECK-NOT: __weavec_rt_bounds_fail
// CHECK: call {{.*}} @__weavec_rt_guard(
// CHECK: ret
int first(struct pair p) {
  int *q = make(p, 4);
  return q != 0 ? q[2] : 0;
}

// CHECK-NOT: pick.weavec.safe
int pick(struct pair p, const int *WEAVEC_COUNTED_BY(n) a, size_t n) {
  return n > 1 && p.a > 0 ? a[1] : 0;
}
