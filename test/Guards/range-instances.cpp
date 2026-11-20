// RFC 0050 §2.4: the element of a range-based `for` whose iterator is a
// pointer is trusted in the function the loop is in, not wherever its
// place recurs: an instance of a template over an array reads its elements
// unchecked, and one over a range of the program's own keeps its check.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <weavec.h>

struct Ints;
WEAVEC_GUARDED const int *firstOf(const Ints &);
WEAVEC_GUARDED const int *endOf(const Ints &);
struct Ints {
  [[nodiscard]] const int *begin() const { return firstOf(*this); }
  [[nodiscard]] const int *end() const { return endOf(*this); }
};

template <class R> [[gnu::noinline]] long sum(const R &range) {
  long total = 0;
  for (int value : range)
    total += value;
  return total;
}

long arrays(const int (&four)[4]) { return sum(four); }
long mine(const Ints &ints) { return sum(ints); }

// CHECK-LABEL: define {{.*}} @_Z3sumIA4_iElRKT_(
// CHECK-NOT: __weavec_rt_
// CHECK: ret
// CHECK-LABEL: define {{.*}} @_Z3sumI4IntsElRKT_(
// CHECK: __weavec_rt_
// CHECK: ret
// CHECK-LABEL: define internal void @weavec.fast(
