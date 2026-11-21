// RFC 0051 §2.3: the runtime's blocks are zeroes, and a unit with a safe
// function says so of each allocation call (`allockind("alloc,zeroed")`,
// not the library's builtin), so that the optimizer does not take a fresh
// block's bytes to be undefined and fold a read of one to any value. An
// allocation nothing reads is still removed with its release.
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s --check-prefix=CALLS
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe %s -o %t && %t
#include <stdlib.h>

// CHECK-LABEL: define {{.*}}i64 @first(
// CHECK-NOT: {{[[:space:]]}}undef
// CHECK: ret i64 0
// FULL-LABEL: define {{.*}}i64 @first(
// FULL: call {{.*}}ptr @malloc({{.*}}) #[[ZEROED:[0-9]+]]
// FULL: attributes #[[ZEROED]] = {{{.*}}nobuiltin{{.*}}allockind("alloc,zeroed")
long first(void) {
  long *q = malloc(4 * sizeof *q);
  if (!q)
    return 1;
  long v = q[3];
  free(q);
  return v;
}

// CALLS-LABEL: define {{.*}}void @unused(
// CALLS-NOT: call
// CALLS: ret void
void unused(void) {
  long *q = malloc(32);
  if (q)
    q[0] = 1;
  free(q);
}

int main(void) { return first() == 0 ? 0 : 1; }
