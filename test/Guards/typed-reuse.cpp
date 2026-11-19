// RFC 0045 §3: in a fast safe C++ program the block a std::unique_ptr
// releases is the next block of its type. RFC 0048 §2.1: a raw pointer
// member to it is believed when it is loaded, so its use before the reuse
// reads the released block, zeroed, and after the reuse the object of the
// same type that lives there. The checked build reports the use (O4).
// RFC 0050 §3.1 (D3): the pooled slot keeps its object's shadow, so that
// guarded code reads it as the zeroed object it is, too; the checked build
// reports that use, and so does a program with a unit in verify mode.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s --check-prefix=REUSE
// RUN: %t stale | FileCheck %s --check-prefix=STALE
// RUN: %t guarded | FileCheck %s --check-prefix=GUARDED
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-safe-checks=full %s -o %t.full
// RUN: not --crash %t.full stale 2>&1 | FileCheck %s
// RUN: not --crash %t.full guarded 2>&1 | FileCheck %s --check-prefix=PEEK
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-checks=verify %s -o %t.verify
// RUN: not --crash %t.verify guarded 2>&1 | FileCheck %s --check-prefix=PEEK
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#include <weavec.h>

struct Item {
  int v = 0;
  long w = 0;
};

struct Cache {
  std::vector<std::unique_ptr<Item>> items;
  const Item *last = nullptr;
  [[gnu::noinline]] void remember() { last = items.back().get(); }
  [[gnu::noinline]] int recall() const { return last->v; }
};

[[gnu::noinline]] void drop(Cache &cache) { cache.items.pop_back(); }
WEAVEC_GUARDED bool asked(int argc, char **argv, const char *mode) {
  return argc > 1 && std::strcmp(argv[1], mode) == 0;
}
WEAVEC_GUARDED [[gnu::noinline]] int peek(const Cache &cache) {
  return cache.last->v;
}

int main(int argc, char **argv) {
  Cache cache;
  cache.items.push_back(std::make_unique<Item>());
  cache.items.back()->v = 7;
  cache.remember();
  drop(cache);
  if (asked(argc, argv, "guarded")) {
    // GUARDED: guarded 0
    // PEEK: weavec: heap-use-after-free at {{.*}}typed-reuse.cpp:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
    std::printf("guarded %d\n", peek(cache));
    return 0;
  }
  if (asked(argc, argv, "stale")) {
    // STALE: stale 0
    // CHECK: weavec: heap-use-after-free at {{.*}}typed-reuse.cpp:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
    std::printf("stale %d\n", cache.recall());
    return 0;
  }
  cache.items.push_back(std::make_unique<Item>());
  cache.items.back()->v = 9;
  // REUSE: 9
  std::printf("%d\n", cache.recall());
  return 0;
}
