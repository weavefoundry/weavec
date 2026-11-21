// RFC 0051 §2.3: a block `new` gives is not said to be undefined (the call
// is not the builtin `new`, whose bytes LLVM takes to be undefined, and its
// `allockind` says neither `uninitialized` nor `zeroed`): a fresh array's
// elements and a member its constructor leaves unset are loaded, and read
// as the runtime's zeroes, in either mode.
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++23 -O2 -fweavec-safe %s -o %t && %t
#include <memory>

// CHECK-LABEL: define {{.*}}i64 @_Z5firstv(
// CHECK-NOT: {{[[:space:]]}}undef
// CHECK: %[[FIRST:[0-9a-z.]+]] = load i64, ptr %
// CHECK: ret i64 %[[FIRST]]
long first() {
  std::unique_ptr<long[]> q = std::make_unique_for_overwrite<long[]>(4);
  return q[3];
}

struct Node {
  long key;
  long value;
  Node() : key(1) {}
};

// CHECK-LABEL: define {{.*}}i64 @_Z6memberv(
// CHECK-NOT: {{[[:space:]]}}undef
// CHECK: %[[MEMBER:[0-9a-z.]+]] = load i64, ptr %
// CHECK: ret i64 %[[MEMBER]]
long member() {
  std::unique_ptr<Node> n = std::make_unique<Node>();
  return n->value;
}

int main() { return first() == 0 && member() == 0 ? 0 : 1; }
