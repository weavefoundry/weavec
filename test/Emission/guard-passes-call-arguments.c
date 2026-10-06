// RFC 0034 §1.2: the guard of a library call's argument whose need is a
// constant (`memcpy(&v, p, 4)`, the idiom of an unaligned load) is lowered
// like an access's guard: expanded inline by the backend, not a call of the
// runtime's out-of-line `__weavec_rt_object`. A need known only at run time
// keeps the call.
//
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

#include <string.h>

// CHECK-LABEL: define {{.*}}i32 @load32(
// CHECK-NOT: __weavec_rt_object
// CHECK: load i8
// CHECK: ret i32
unsigned load32(const unsigned char *p) {
  unsigned v;
  memcpy(&v, p, sizeof v);
  return v;
}

// CHECK-LABEL: define {{.*}}i32 @same(
// CHECK: call {{.*}}@__weavec_rt_object
// CHECK: ret i32
int same(const unsigned char *a, const unsigned char *b, unsigned long n) {
  return memcmp(a, b, n) == 0;
}
