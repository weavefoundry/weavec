// RFC 0039 §8: unsafe regions in C++. WEAVEC_UNSAFE before a member
// function, a function in a namespace, a block, a lambda's body inside an
// unsafe block, and a template's definition (in every instantiation), and
// no_sanitize("address") on a member function: none of their accesses is
// guarded. The rest are.
// RUN: rm -rf %t.dir && mkdir -p %t.dir
// RUN: %weavec_cxx -O2 -c %s -o %t.dir/unsafe.o -fweavec-ledger=%t.dir/ -fweavec-summary 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: FileCheck %s --check-prefix=LEDGER < %t.dir/unsafe.o.ledger.json
#include <weavec.h>

namespace shapes {
struct Grid {
  WEAVEC_UNSAFE int peek(const int *p, int i) const { return p[i]; }
  [[clang::no_sanitize("address")]] int scan(const int *p, int i) const {
    return p[i];
  }
  int checked(const int *p, int i) const { return p[i]; }
};

WEAVEC_UNSAFE int raw(const int *p, int i) { return p[i]; }

template <typename T> WEAVEC_UNSAFE T at(const T *p, int i) { return p[i]; }
} // namespace shapes

int use(const int *p, const long *q, int i) {
  shapes::Grid grid;
  int s = grid.peek(p, i) + grid.scan(p, i + 1) + grid.checked(p, i + 2) +
          shapes::raw(p, i + 3) + static_cast<int>(shapes::at(q, i)) +
          shapes::at(p, i + 4);
  WEAVEC_UNSAFE {
    auto twice = [&](int j) { return p[j] * 2; };
    s += twice(i + 5);
  }
  return s;
}

// (Each instantiation of the template has its rows.)
// SUMMARY: weavec: {{.*}}unsafe-region.cpp: 9 accesses: 0 proven, 1 guarded, 8 unguarded
// LEDGER-DAG: "outcome": "unguarded"
// LEDGER-DAG: "reason": "unsafe"
