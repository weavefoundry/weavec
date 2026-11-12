// RFC 0042 §4, §5, §7: a typed body reads no shadow through owners (RFC
// 0043 §7: a reference field is tested where it is loaded). A counted field's
// element is compared by its index with the count loaded beside the
// pointer; a single field's member at a constant offset is not checked at
// all; a string is walked under the loop's own test. The field annotations
// the checker added are gone before the optimizer runs; the body of an
// external typed function is external, and a typed unit registers its
// program's defaults.
// RUN: %weavec_cc -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <weavec.h>
#include <stddef.h>

struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t cap; };
struct node { struct node *WEAVEC_OWNED next; int value; };

// CHECK-NOT: llvm.ptr.annotation
// CHECK: @llvm.global_ctors = {{.*}}@weavec.typed

// CHECK-LABEL: define {{.*}}i32 @get.weavec.safe(ptr {{.*}}%v, i64 {{.*}}%i)
// CHECK-NOT: __weavec_rt_shadow
// CHECK: %[[COUNT:.*]] = load i64
// CHECK: select i1 %{{.*}}, i64 0, i64 %[[COUNT]]
// CHECK: icmp ult i64 %i,
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret i32
int get(const struct vec *v, size_t i) { return v->data[i]; }

// CHECK-LABEL: define {{.*}}i32 @second.weavec.safe(
// CHECK-NOT: __weavec_rt_shadow
// CHECK-NOT: icmp
// CHECK-NOT: __weavec_rt_shadow
// CHECK: ret i32
int second(const struct node *n) { return n->next->value; }

// CHECK-LABEL: define {{.*}}i64 @length.weavec.safe(
// CHECK-NOT: __weavec_rt
// CHECK: ret i64
size_t length(const char *WEAVEC_NULL_TERMINATED s) {
  size_t n = 0;
  for (; *s; s++)
    n++;
  return n;
}

// The entries check what code that is not typed passes, the struct's bytes
// and the string's terminator, then run the body (here inlined).
// CHECK-LABEL: define {{.*}}i32 @get(
// CHECK: __weavec_rt_shadow
// CHECK-LABEL: define {{.*}}i64 @length(
// CHECK: @__weavec_rt_strlen(
