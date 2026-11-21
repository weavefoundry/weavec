// RFC 0045 §2: Clang names a type in an allocation's `!alloc_token`
// without the function it is declared in, so two functions' `struct Node`
// print alike. A fast unit does not type an allocation whose type's name
// another of its types shares: neither block goes to a pool, and a stale
// pointer into one cannot reach the other type's object. A local type no
// other type shares is typed, keyed by its unit too.
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s --check-prefix=OUT
#include <bit>
#include <cstdio>
#include <memory>
#include <weavec.h>

static const char Secret[] = "secret";
struct Keep {
  long *p = nullptr;
};
void keep(Keep &k, long &WEAVEC_ESCAPES x) { k.p = &x; }

// CHECK-LABEL: define {{.*}}@_Z5staleR4Keep(
// CHECK-NOT: __weavec_rt_new_typed
// CHECK: call {{.*}}@_Znwm(
void stale(Keep &k) {
  struct Node {
    long x = 7;
    long y = 8;
  };
  auto a = std::make_unique<Node>();
  keep(k, a->x);
} // a's block is released; were it pooled, it would be the next Node's

[[gnu::noinline]] void forge(Keep &k) {
  *k.p = std::bit_cast<long>(&Secret[0]);
}

// CHECK-LABEL: define {{.*}}@_Z6victimR4Keep(
// CHECK-NOT: __weavec_rt_new_typed
// CHECK: call {{.*}}@_Znwm(
// OUT: name = public
void victim(Keep &k) {
  struct Node {
    const char *name = "public";
    long y = 9;
  };
  auto b = std::make_unique<Node>();
  std::printf("b at %p\n", static_cast<void *>(b.get()));
  forge(k); // writes through the stale long* to a's block
  std::printf("name = %s\n", b->name);
}

// CHECK-LABEL: define {{.*}}@_Z4lonev(
// CHECK: call {{.*}}@__weavec_rt_new_typed(
long lone() {
  struct Lone {
    long v = 3;
  };
  auto c = std::make_unique<Lone>();
  return c->v;
}

int main() {
  Keep k;
  stale(k);
  victim(k);
  return lone() == 3 ? 0 : 1;
}
