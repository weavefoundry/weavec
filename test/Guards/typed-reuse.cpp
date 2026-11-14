// RFC 0045 §3, §4: in a fast safe C++ program the block a std::unique_ptr
// releases is the next block of its type; a raw pointer member to it is a
// reference, tested when it is loaded, so its use before the reuse is
// still reported, without allocation stacks and with a note; after the
// reuse it reads the object of the same type that lives there.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s --check-prefix=REUSE
// RUN: not --crash %t stale 2>&1 | FileCheck %s
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
WEAVEC_GUARDED bool stale(int argc, char **argv) {
  return argc > 1 && std::strcmp(argv[1], "stale") == 0;
}

int main(int argc, char **argv) {
  Cache cache;
  cache.items.push_back(std::make_unique<Item>());
  cache.items.back()->v = 7;
  cache.remember();
  drop(cache);
  if (stale(argc, argv))
    // CHECK: weavec: heap-use-after-free at {{.*}}typed-reuse.cpp:{{[0-9]+}}:{{[0-9]+}}: read of 4 bytes
    // CHECK: weavec: note: built for speed
    return cache.recall();
  cache.items.push_back(std::make_unique<Item>());
  cache.items.back()->v = 9;
  // REUSE: 9
  std::printf("%d\n", cache.recall());
  return 0;
}
