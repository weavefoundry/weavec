// RFC 0051 §2.3: a unit with a safe function says of no allocation call
// that its block's bytes are undefined (its `allockind` says neither
// `uninitialized` nor `zeroed`, and it is not the library's builtin), so
// that the optimizer does not fold a read of a fresh block to any value: it
// loads the runtime's zeroes. Nor does it say zeroed (RFC 0051 amendment
// 18): a program's own allocator serves the call in place of the runtime's,
// and a program's own zeroing of its block stays. An allocation nothing
// reads is still removed with its release.
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s --check-prefix=CALLS
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cc -std=c23 -O2 -fweavec-safe %s -o %t && %t
// RUN: rm -rf %t.own && mkdir -p %t.own
// RUN: %clang -O1 -c %S/Inputs/fresh-blocks-allocator.c -o %t.own/alloc.o
// RUN: %weavec_cc -std=c23 -O2 %s %t.own/alloc.o -o %t.own/prog 2>/dev/null
// RUN: %t.own/prog own
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

// CHECK-LABEL: define {{.*}}i64 @first(
// CHECK-NOT: {{[[:space:]]}}undef
// CHECK: load i64, ptr %
// CHECK: ret i64
// FULL-LABEL: define {{.*}}i64 @first(
// FULL: call {{.*}}ptr @malloc({{.*}}) #[[KIND:[0-9]+]]
// FULL: attributes #[[KIND]] = {{{.*}}nobuiltin{{.*}}allockind("alloc")
long first(void) {
  long *q = malloc(4 * sizeof *q);
  if (!q)
    return 1;
  long v = q[3];
  free(q);
  return v;
}

// A program's zeroing of a fresh block, which its own allocator needs (in
// guarded code of the unit, which the attributes cover too).
// CHECK-LABEL: define {{.*}}ptr @cleared(
// CHECK: {{call .*@(llvm\.memset|calloc)}}
WEAVEC_GUARDED __attribute__((noinline)) long *cleared(void) {
  long *p = malloc(2 * sizeof *p);
  if (p)
    memset(p, 0, 2 * sizeof *p);
  return p;
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

// The unit's safe function: without -fweavec-safe, the one that makes the
// unit's allocation calls say nothing of their blocks' bytes.
WEAVEC_SAFE long none(void) { return 0; }

WEAVEC_GUARDED int main(int argc, char **argv) {
  (void)argv;
  long *p = cleared();
  if (p == NULL || p[0] != 0 || p[1] != 0)
    return 1;
  // With the program's own allocator, a fresh block is not zeroes.
  return argc > 1 || first() == none() ? 0 : 1;
}
