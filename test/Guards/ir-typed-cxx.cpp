// RFC 0048 §2.1, §3, §5.2, §6.4: a fast safe C++ unit checks its code
// against what it holds. An access at a constant offset through a pointer
// it loaded (whose type tag names the member) has no guard, nor has a
// virtual call's vtable and slot; a string position stepped over bytes a
// call compared is checked where it is stepped, and its reads have no
// guard; a C library call on strings it holds is the library's own; a
// local only safe code sees is in a plain frame. The checked build keeps
// every guard.
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
#include <cstring>
#include <weavec.h>

struct WEAVEC_ALIASED Node {
  virtual ~Node();
  virtual int weight() const;
  Node *next = nullptr;
  int value = 0;
};

// CHECK-LABEL: define {{.*}}@_Z6secondPK4Node.weavec.safe(
// CHECK-NOT: __weavec_rt_guard
// CHECK: {{^}$}}
// FULL-LABEL: define {{.*}}@_Z6secondPK4Node(
// FULL: __weavec_rt_guard
int second(const Node *n) { return n->next != nullptr ? n->next->value : 0; }

// CHECK-LABEL: define {{.*}}@_Z5totalRK4Node(
// CHECK-NOT: __weavec_rt_guard
// CHECK: {{^}$}}
int total(const Node &n) { return n.next != nullptr ? n.next->weight() : 0; }

// CHECK-LABEL: define {{.*}}@_Z6headerPKc.weavec.safe(
// CHECK: call {{.*}}@strncmp(
// CHECK-NOT: __weavec_rt_strncmp
// CHECK: icmp eq i8 {{.*}}, 0
// CHECK: __weavec_rt_bounds_fail
// CHECK-NOT: __weavec_rt_guard
// CHECK: {{^}$}}
// FULL-LABEL: define {{.*}}@_Z6headerPKc(
// FULL: __weavec_rt_strncmp
const char *WEAVEC_NULL_TERMINATED header(const char *WEAVEC_NULL_TERMINATED p) {
  if (std::strncmp(p, "<!--", 4) == 0)
    p += 4;
  while (*p == ' ')
    ++p;
  return p;
}

__attribute__((noinline)) int sum(const int *WEAVEC_COUNTED_BY(n) a, int n) {
  int s = 0;
  for (int i = 0; i < n; ++i)
    s += a[i];
  return s;
}

// CHECK-LABEL: define {{.*}}@_Z4filli(
// CHECK-NOT: weavec.frame
// CHECK: call {{.*}}@_Z3sumPKii.weavec.safe(
// CHECK: {{^}$}}
// FULL-LABEL: define {{.*}}@_Z4filli(
// FULL: %weavec.frame = alloca
int fill(int k) {
  int a[16];
  for (int i = 0; i < 16; ++i)
    a[i] = i * k;
  return sum(a, 16);
}
