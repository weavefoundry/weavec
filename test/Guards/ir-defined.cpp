// RFC 0051 §2.2, §3.1: a unit compiled with -fweavec-safe defines its
// arithmetic in every function it emits, the library code it instantiates
// included (no `nsw`, saturating conversions, no `mustprogress`); the end
// of a function that must return a value is Clang's check, whose handler is
// the runtime's; `std::unreachable()` (libc++'s) is a stop.
// RUN: %weavec_cxx -std=c++23 -O1 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// Without the flag, a unit's code is Clang's.
// RUN: %weavec_cxx -std=c++23 -O1 -S -emit-llvm -Wno-return-type %s -o - | FileCheck %s --check-prefix=PLAIN
#include <numeric>
#include <utility>
#include <vector>

// libstdc++'s std::unreachable() under its assertions traps by itself:
// there, the builtin it is made of.
#ifdef _LIBCPP_VERSION
#define UNREACHABLE std::unreachable
#else
#define UNREACHABLE __builtin_unreachable
#endif

enum class Op { Add, Sub };

// CHECK-LABEL: define {{.*}}@_Z4step2Opii(
// CHECK: call void @__ubsan_handle_missing_return(
// PLAIN-LABEL: define {{.*}}@_Z4step2Opii(
// PLAIN-NOT: __ubsan_handle_missing_return
// PLAIN: ret
int step(Op op, int a, int b) {
  switch (op) {
  case Op::Add:
    return a + b;
  case Op::Sub:
    return a - b;
  }
}

// CHECK-LABEL: define {{.*}}@_Z5indexi(
// CHECK: call void @__weavec_rt_unreachable(
int index(int c) {
  switch (c) {
  case 0:
    return 3;
  case 1:
    return 7;
  }
  UNREACHABLE();
}

// The library's code, instantiated here, wraps too.
// CHECK-LABEL: define {{.*}}@_Z3sumRK{{.*}}(
// CHECK-NOT: add nsw
// CHECK: ret
int sum(const std::vector<int> &v) { return std::accumulate(v.begin(), v.end(), 0); }

// CHECK-LABEL: define {{.*}}@_Z7convertd(
// CHECK: @llvm.fptosi.sat.i32.f64(
int convert(double d) { return static_cast<int>(d); }

// A loop with no effect may run forever: it is kept, and its metadata says
// nothing of progress (the optimizer still infers it of what returns).
// CHECK-LABEL: define {{.*}}@_Z4spini(
// CHECK: br
int spin(int n) {
  while (n != 7)
    n += 2;
  return n;
}
// CHECK-NOT: !"llvm.loop.mustprogress"
