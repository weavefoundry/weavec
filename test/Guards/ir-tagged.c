// RFC 0054 §3.3: in a safe function a tagged union's member access is
// preceded by a load of the tag and its comparison with the member's values
// (`__weavec_rt_union_fail` when it fails), and a store of the tag clears the
// union when the value changes; a guarded function's accesses are as Clang
// emits them, and no annotation of the union is left in the code.
// RUN: %weavec_cc -O0 -S -emit-llvm %s -o - | FileCheck %s
#include <weavec.h>

enum kind { KIND_NUMBER, KIND_TEXT, KIND_NAME };
struct value {
  enum kind kind;
  union {
    double number WEAVEC_WHEN(KIND_NUMBER);
    const char *text WEAVEC_WHEN(KIND_TEXT, KIND_NAME);
  } as WEAVEC_TAGGED_BY(kind);
};

// CHECK: @[[MEMBER:__weavec.member[.0-9]*]] = private constant { i64, i64, i32, i32 } {{.*}}, i32 0, i32 0 }

// CHECK-LABEL: define {{.*}} @number.weavec.safe(
// CHECK: %[[TAG:[0-9]+]] = load i32, ptr
// CHECK: %[[WIDE:[0-9]+]] = zext i32 %[[TAG]] to i64
// CHECK: %[[MASKED:[0-9]+]] = and i64 %[[WIDE]], 4294967295
// CHECK: icmp eq i64 %[[MASKED]], 0
// CHECK: call void @__weavec_rt_union_fail(i64 %[[MASKED]], ptr @[[MEMBER]], ptr @__weavec.site
// CHECK: load double, ptr %as
WEAVEC_SAFE double number(const struct value *v) { return v->as.number; }

// Two values: two comparisons.
// CHECK-LABEL: define {{.*}} @text.weavec.safe(
// CHECK: icmp eq i64 %{{[0-9]+}}, 1
// CHECK: icmp eq i64 %{{[0-9]+}}, 2
// CHECK: call void @__weavec_rt_union_fail(
WEAVEC_SAFE const char *text(const struct value *v WEAVEC_LIFETIMEBOUND) { return v->as.text; }

// CHECK-LABEL: define {{.*}} @retag.weavec.safe(
// CHECK: %[[OLD:[0-9]+]] = load i32, ptr %kind
// CHECK: store i32 0, ptr %kind
// CHECK: icmp ne i32 %{{[0-9]+}}, 0
// CHECK: call void @llvm.memset.p0.i64(ptr {{.*}}, i8 0, i64 8, i1 false)
WEAVEC_SAFE void retag(struct value *v) { v->kind = KIND_NUMBER; }

// An initializer writes the tag and the member together: no check, no
// clearing of the union's 8 bytes (the local's own zeroing stays).
// CHECK-LABEL: define {{.*}} @make(
// CHECK-NOT: __weavec_rt_union_fail
// CHECK-NOT: i8 0, i64 8, i1 false)
// CHECK: ret
WEAVEC_SAFE double make(double d) {
  struct value v = {.kind = KIND_NUMBER, .as.number = d};
  return v.kind == KIND_NUMBER ? d : 0;
}

// CHECK-LABEL: define {{.*}} @guarded(
// CHECK-NOT: __weavec_rt_union_fail
// CHECK: ret double
double guarded(const struct value *v) { return v->as.number; }

// CHECK-NOT: llvm.ptr.annotation
