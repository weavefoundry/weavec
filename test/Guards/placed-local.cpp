// RFC 0048 §5.1: a local whose address a safe C++ function keeps lives in
// typed memory for its scope. A pointer kept past the scope reaches, in the
// fast build, memory of the local's type (here the next local placed: the
// stale pointer adds to it, no other type), and stops with a report in the
// checked build; a local of a WEAVEC_ALIASED class is placed by its type,
// in guarded code too. The summary line counts the placed locals.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-summary -c %s -o %t.o 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: %weavec_cxx %t.o -o %t
// RUN: %t | FileCheck %s --check-prefix=FAST
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full 2>&1 | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=GUARDED
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NS
#include <cstdio>
#include <vector>
#include <weavec.h>

struct Counter {
  long hits = 0;
  long pad[3] = {};
};
struct Name {
  const char *text = "ok";
  long pad[3] = {};
};
struct Registry {
  std::vector<Counter *> counters;
  void add(Counter &WEAVEC_ESCAPES c) { counters.push_back(&c); }
  void bump() {
    for (Counter *c : counters)
      c->hits += 0x1000000;
  }
};
[[gnu::noinline]] void use(Counter &c) { c.hits += 1; }
[[gnu::noinline]] void use(Name &n) { n.pad[0] += 1; }

// SUMMARY: placed locals
[[gnu::noinline]] void enroll(Registry &r) {
  Counter local;
  use(local);
  r.add(local);
}
// CHECK: weavec: heap-use-after-free at {{.*}}placed-local.cpp:[[@LINE-12]]
[[gnu::noinline]] const char *victim(Registry &r) {
  Name name;
  use(name);
  r.bump();
  return name.text;
}

struct WEAVEC_ALIASED Tree {
  Tree *parent = nullptr;
};
// GUARDED-LABEL: define {{.*}}@_Z5plantv(
// GUARDED: call {{.*}}@calloc(
// GUARDED: call void @__weavec_rt_free_at(
WEAVEC_GUARDED long plant() {
  Tree tree;
  return tree.parent != nullptr;
}

// A class whose constructor makes a member that keeps its object (a small
// buffer's own pointer) keeps its object too, said or not: guarded code's
// local is placed.
struct Small {
  long pool[4] = {};
  long *mem;
  Small() WEAVEC_ESCAPES : mem(pool) {}
};
struct Holder {
  Small inner;
  WEAVEC_GUARDED Holder() : inner() {}
};
// GUARDED-LABEL: define {{.*}}@_Z4holdv(
// GUARDED: call {{.*}}@calloc(
WEAVEC_GUARDED long hold() {
  Holder holder;
  return *holder.inner.mem;
}

// A function in a namespace is marked before its code is made too.
// NS-LABEL: define {{.*}}@_ZN6garden3sowEv(
// NS: call {{.*}}@__weavec_rt_take(
// NS: call void @__weavec_rt_give(
namespace garden {
long sow() {
  Tree tree;
  return tree.parent != nullptr;
}
} // namespace garden

// A local of a class whose constructor keeps its object is placed however
// it is made, and so is one whose implicit constructor points a member
// into it.
struct Tagged {
  long pool[4] = {};
  long *mem;
  explicit Tagged(long first) WEAVEC_ESCAPES : mem(pool) { pool[0] = first; }
};
struct Pointed {
  long pool[4] = {};
  long *mem = pool;
};
// NS-LABEL: define {{.*}}@_Z4madev(
// NS: call {{.*}}@__weavec_rt_take(
// NS: call {{.*}}@__weavec_rt_take(
long made() {
  auto tagged = Tagged(3);
  Pointed pointed;
  return *tagged.mem + *pointed.mem;
}

int main() {
  Registry r;
  enroll(r);
  const char *text = victim(r);
  // FAST: ok
  std::printf("%s\n", text);
  return static_cast<int>(plant() + hold());
}
