// RFC 0045 §4: in a fast safe C++ unit an access at a constant offset
// inside what a reference parameter or `this` covers carries no guard; an
// access through an iterator, or at a variable offset, keeps its guard.
// Verify mode keeps each removed guard as a monitor whose site says it is
// believed (32 in its flags, beside the monitor's 4). The ledger names the
// reason. Under -fweavec-safe-checks=full every guard stays.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-checks=verify -S -emit-llvm %s -o - | FileCheck %s --check-prefix=VERIFY
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -c %s -o %t.o -fweavec-ledger=%t.json
// RUN: FileCheck %s --check-prefix=LEDGER < %t.json
#include <vector>

struct Point {
  long x = 0;
  long y = 0;
};

// CHECK-LABEL: define {{.*}} @_Z3sumRK5Point(
// CHECK-NOT: call {{.*}}@__weavec_rt_guard
// CHECK: ret
// FULL-LABEL: define {{.*}} @_Z3sumRK5Point(
// FULL: __weavec_rt_shadow
// VERIFY: @[[MONITOR:__weavec.site[.0-9]*]] = private unnamed_addr constant { i64, i32, i16, i16 } {{.*}}, i16 36 }
// VERIFY-LABEL: define {{.*}} @_Z3sumRK5Point(
// VERIFY: call {{.*}}@__weavec_rt_guard{{.*}}(i64 {{.*}}, i64 8, ptr @[[MONITOR]])
[[gnu::noinline]] long sum(const Point &p) { return p.x + p.y; }

// CHECK-LABEL: define {{.*}} @_Z5total{{[^(]*}}(
// CHECK: __weavec_rt_shadow
[[gnu::noinline]] long total(const std::vector<long> &values) {
  long s = 0;
  for (auto it = values.begin(); it != values.end(); ++it)
    s += *it;
  return s;
}

// LEDGER: "reason": "believed"
