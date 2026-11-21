// RFC 0051 §6: WeaveC's own sanitizer kinds are added whatever the command
// line asks for: a fast unit compiled with a UBSan check keeps its typed
// allocations (before, any -fsanitize= switched them off without a word).
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-summary -fsanitize=return -fsanitize-trap=return -c %s -o %t.o 2>&1 | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-summary -c %s -o %t.plain.o 2>&1 | FileCheck %s
// CHECK: weavec: {{.*}}safe-sanitizers.cpp: {{.*}}; 1 typed allocation sites
#include <memory>

struct Node {
  long value = 0;
  std::unique_ptr<Node> next;
};

std::unique_ptr<Node> make(long value) {
  auto node = std::make_unique<Node>();
  node->value = value;
  return node;
}
