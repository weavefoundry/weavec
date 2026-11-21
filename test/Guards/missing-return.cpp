// RFC 0051 §3.1: the end of a function that must return a value, reached,
// stops (`missing-return`): a switch over every value of an `enum class`
// that is given another value no longer jumps through a table without a
// range check. The checked build stops the same way.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe %s -o %t
// RUN: %t 1 | FileCheck %s --check-prefix=OK
// RUN: not --crash %t 40 2>&1 | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O0 -fweavec-safe %s -o %t.O0
// RUN: not --crash %t.O0 40 2>&1 | FileCheck %s
#include <cstdio>
#include <cstdlib>
#include <vector>

enum class Op { Add, Sub, Mul, Neg };

struct Machine {
  std::vector<long> stack{1};
  long acc = 0;
  // CHECK: weavec: missing-return at {{.*}}missing-return.cpp:[[@LINE+1]]:{{[0-9]+}}: the function ended without returning a value
  __attribute__((noinline)) long step(Op op, long x) {
    switch (op) {
    case Op::Add:
      return acc += x;
    case Op::Sub:
      return acc -= x;
    case Op::Mul:
      return acc *= x;
    case Op::Neg:
      return acc = -acc;
    }
  }
};

int main(int argc, char **argv) {
  Machine m;
  const int op = argc > 1 ? std::atoi(argv[1]) : 0;
  // OK: -1
  std::printf("%ld\n", m.step(static_cast<Op>(op), 1));
  return 0;
}
