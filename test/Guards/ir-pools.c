// RFC 0045 §2, §3, RFC 0055 §5, §7: in a unit compiled as safe in the
// fast mode (the default), an allocation whose type Clang infers calls the
// runtime's short path, `take`, with a record of the type and its form,
// and a safe function's release (its public entry's too) calls `give` with
// its size (all ones: not sized), its form and the site flag that returns
// the block to its pool (16). RFC 0055 §3: a `realloc` keeps its block's
// type (`realloc_typed`, with the site's record). A guarded function's
// allocation and release stay as they are; -fweavec-safe-checks=full types
// nothing. The unit tells the runtime it is fast.
// RUN: %weavec_cc -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s --implicit-check-not=__weavec_rt_tests_released
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-safe-checks=full -S -emit-llvm %s -o - | FileCheck %s --check-prefixes=FULL,CHECKED
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=FULL
#include <stdlib.h>
#include <weavec.h>

struct node {
  int v;
  long w;
};

// RFC 0055 §2 (D2): a fast unit of Safe C keeps its pooled slots' shadow,
// as one of Safe C++ does (it calls no `tests_released`); a checked unit,
// whose reference test reads a released block's shadow, tells the runtime
// to mark pooled slots released.
// CHECKED: @llvm.global_ctors = {{.*}}@weavec.released
// CHECK-DAG: @[[TYPE:__weavec.type[.0-9]*]] = private global { i64, i64, ptr, i64 } { i64 {{-?[0-9]+}}, i64 0, ptr @__weavec.layout.{{[0-9A-F]+}}, i64 0 }, align 16
// CHECK-DAG: @[[TYPED:__weavec.site[.0-9]*]] = private unnamed_addr constant { i64, i32, i16, i16 } {{.*}}, i16 16 }
// CHECK-LABEL: define {{.*}} @make.weavec.safe(
// CHECK: call noalias ptr @__weavec_rt_take(i64 16, ptr @__weavec.type{{[.0-9]*}}, i32 0)
// FULL-LABEL: define {{.*}} @make(
// FULL-NOT: _take
// FULL: call {{.*}}@malloc(
struct node *WEAVEC_OWNED make(int v) {
  struct node *n = malloc(sizeof *n);
  if (n != NULL)
    n->v = v;
  return n;
}

// CHECK-LABEL: define {{.*}} @drop.weavec.safe(
// CHECK: call void @__weavec_rt_give(ptr {{.*}}, i64 -1, i32 0, ptr @[[TYPED]])
void drop(struct node *WEAVEC_OWNED n) { free(n); }

// CHECK-LABEL: define {{.*}} @grow.weavec.safe(
// CHECK: call noalias ptr @__weavec_rt_realloc_typed(ptr {{.*}}, i64 {{.*}}, ptr @__weavec.type{{[.0-9]*}}, ptr @__weavec.site{{[.0-9]*}})
// FULL-LABEL: define {{.*}} @grow(
// FULL-NOT: realloc_typed
// FULL: call ptr @__weavec_rt_realloc_at(
struct node *WEAVEC_OWNED grow(struct node *WEAVEC_OWNED nodes, size_t n) {
  return realloc(nodes, n * sizeof *nodes);
}

// CHECK-LABEL: define {{.*}} @raw(
// CHECK-NOT: _take
// CHECK: call {{.*}}@malloc(
WEAVEC_GUARDED void *raw(size_t n) { return malloc(n); }

// CHECK-LABEL: define internal void @weavec.fast(
// CHECK: call void @__weavec_rt_fast_program()
// The public entry of the split function releases as its body does.
// CHECK-LABEL: define {{.*}} @drop(
// CHECK: call void @__weavec_rt_give(ptr {{.*}}, i64 -1, i32 0, ptr @[[TYPED]])
// FULL-NOT: fast_program
