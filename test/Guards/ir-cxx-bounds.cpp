// RFC 0047 §1.4: in a safe C++ function an access through a pointer with a
// bound is a comparison with the bound, not a guard, and its public entry
// checks the bound against the shadow; a function that uses no bound
// compiles as before, and its accesses keep their guards.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <cstddef>
#include <weavec.h>

// CHECK-LABEL: define {{.*}}@_Z3sumPKim.weavec.safe(
// CHECK-NOT: __weavec_rt_guard
// CHECK: call {{.*}}@__weavec_rt_bounds_fail
// CHECK-NOT: __weavec_rt_guard
// CHECK: {{^}$}}
int sum(const int *WEAVEC_COUNTED_BY(n) v, std::size_t n) {
  int s = 0;
  for (std::size_t i = 0; i <= n; ++i)
    s += v[i];
  return s;
}

struct Node {
  Node *next;
  int v;
};
// CHECK-LABEL: define {{.*}}@_Z5valueRK4Node(
// CHECK-NOT: __weavec_rt_bounds_fail
// CHECK: call {{.*}}@__weavec_rt_guard
// CHECK: {{^}$}}
int value(const Node &n) { return n.next != nullptr ? n.next->v : n.v; }

// CHECK-LABEL: define {{.*}}@_Z3sumPKim(
// CHECK: call void @__weavec_rt_range(
// CHECK: {{^}$}}
