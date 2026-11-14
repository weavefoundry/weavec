// RFC 0045 §2, §3: in a unit compiled as safe in the fast mode (the
// default), an allocation whose type Clang infers calls the runtime's typed
// entry point with a record of the type, and a safe function's release
// (its public entry's too) carries the site flag that returns the block to
// its pool (16). A guarded function's allocation and release stay as they
// are; -fweavec-safe-checks=full types nothing. The unit tells the runtime
// it is fast.
// RUN: %weavec_cc -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
#include <stdlib.h>
#include <weavec.h>

struct node {
  int v;
  long w;
};

// CHECK-DAG: @[[TYPE:__weavec.type[.0-9]*]] = private global { i64, i64 } { i64 {{-?[0-9]+}}, i64 0 }, align 16
// CHECK-DAG: @[[TYPED:__weavec.site[.0-9]*]] = private unnamed_addr constant { i64, i32, i16, i16 } {{.*}}, i16 16 }
// CHECK-LABEL: define {{.*}} @make.weavec.safe(
// CHECK: call noalias ptr @__weavec_rt_alloc_typed(i64 16, i32 0, ptr @__weavec.type
// FULL-LABEL: define {{.*}} @make(
// FULL-NOT: alloc_typed
// FULL: call {{.*}}@malloc(
struct node *WEAVEC_OWNED make(int v) {
  struct node *n = malloc(sizeof *n);
  if (n != NULL)
    n->v = v;
  return n;
}

// CHECK-LABEL: define {{.*}} @drop.weavec.safe(
// CHECK: call void @__weavec_rt_free_at(ptr {{.*}}, ptr @[[TYPED]])
void drop(struct node *WEAVEC_OWNED n) { free(n); }

// CHECK-LABEL: define {{.*}} @raw(
// CHECK-NOT: alloc_typed
// CHECK: call {{.*}}@malloc(
WEAVEC_GUARDED void *raw(size_t n) { return malloc(n); }

// CHECK-LABEL: define internal void @weavec.fast(
// CHECK: call void @__weavec_rt_fast_program()
// The public entry of the split function releases as its body does.
// CHECK-LABEL: define {{.*}} @drop(
// CHECK: call void @__weavec_rt_free_at(ptr {{.*}}, ptr @[[TYPED]])
// FULL-NOT: fast_program
