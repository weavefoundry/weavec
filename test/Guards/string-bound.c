// RFC 0035 §2.5: a bound that is itself a string's length (the `strlen`
// the string guard replaced) compiles; the comparison is the runtime's,
// which checks the bytes it reads.
// RUN: %weavec_cc -O0 -c %s -o %t0.o
// RUN: %weavec_cc -O2 -c %s -o %t2.o
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
#include <string.h>

// CHECK-LABEL: define {{.*}}@prefix(
// CHECK: call i64 @__weavec_rt_strlen(
// CHECK: call i32 @__weavec_rt_strncmp(
int prefix(const char *a, const char *b) { return strncmp(a, b, strlen(a)); }
