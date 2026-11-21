// RFC 0051 §3.1: in a safe function, code the program says cannot be
// reached stops when it is reached (`unreachable`), in the fast and in the
// checked build: `std::unreachable()` after a switch whose result indexes
// an array no longer removes the array's check.
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe %s -o %t
// RUN: %t 2 | FileCheck %s --check-prefix=OK
// RUN: not --crash %t 100 2>&1 | FileCheck %s
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full 100 2>&1 | FileCheck %s
#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <utility>

// libstdc++'s std::unreachable() under its assertions traps by itself,
// before the stop: there, the builtin it is made of.
#ifdef _LIBCPP_VERSION
#define UNREACHABLE std::unreachable
#else
#define UNREACHABLE __builtin_unreachable
#endif

enum class Color { Red, Green, Blue };

struct Palette {
  std::array<long, 3> rgb{1, 2, 3};
  long secret = 0x5ec2e7;
};

static std::size_t indexOf(Color c) {
  switch (c) {
  case Color::Red:
    return 0;
  case Color::Green:
    return 1;
  case Color::Blue:
    return 2;
  }
  // CHECK: weavec: unreachable at {{.*}}unreachable.cpp:[[@LINE+1]]:3: reached code the program says cannot be reached
  UNREACHABLE();
}

__attribute__((noinline)) long channel(const Palette &p, Color c) {
  return p.rgb[indexOf(c)];
}

int main(int argc, char **argv) {
  auto p = std::make_unique<Palette>();
  const Color c = static_cast<Color>(argc > 1 ? std::atoi(argv[1]) : 0);
  // OK: 3
  std::printf("%ld\n", channel(*p, c));
  return 0;
}
