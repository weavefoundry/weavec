// RFC 0048 §7: a safe C++ function's parameter that keeps what it is given
// (WEAVEC_ESCAPES after a reference's `&`) is checked at the entry code that
// is not safe calls: an address in a stack stops there (`stack-escape`). A
// safe caller's local is placed in typed memory instead, and passes.
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe -c %s -o %t.o
// RUN: %weavec_cxx -std=c++20 -O1 -c %S/Inputs/stack-escape-caller.cpp -o %t.caller.o
// RUN: %weavec_cxx %t.o %t.caller.o -o %t
// RUN: %t safe
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <cstring>
#include <vector>
#include <weavec.h>

struct Counter {
  long hits = 0;
};
struct Registry {
  std::vector<Counter *> counters;
};
[[gnu::noinline]] void enroll(Registry &r, Counter &WEAVEC_ESCAPES c) {
  r.counters.push_back(&c);
}

[[gnu::noinline]] long fromSafe() {
  Registry r;
  Counter mine;
  enroll(r, mine);
  return mine.hits;
}

long fromGuarded();
// CHECK: weavec: stack-escape at {{.*}}stack-escape-entry.cpp:[[@LINE-12]]
// CHECK: in fromGuarded() {{.*}}stack-escape-caller.cpp:
int main(int argc, char **argv) {
  if (argc > 1)
    return static_cast<int>(fromSafe());
  return static_cast<int>(fromGuarded());
}
