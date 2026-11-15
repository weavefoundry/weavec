// RFC 0045 §3: the functions the compiler makes of a fast unit's own code
// (here the destructor of a `thread_local` array, into which the guarded class's
// destructor is inlined) release as that code does: a guarded class's
// `delete` keeps its block out of the pools wherever it runs. Nothing in
// this unit releases from safe code, so no site carries the flag (16).
// RUN: %weavec_cxx -std=c++20 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <weavec.h>

struct WEAVEC_GUARDED Holder {
  int *p;
  Holder() : p(new int(1)) {}
  ~Holder() { delete p; }
};

thread_local Holder table[4];

// CHECK-NOT: i16 16 }
// CHECK-LABEL: define internal void @__cxx_global_array_dtor(
// CHECK: call void @__weavec_rt_delete_sized_at(
int main() { return *table[0].p - 1; }
