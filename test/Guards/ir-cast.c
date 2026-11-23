// RFC 0054 §1, §2.2: in a safe C function a checked conversion is a call of
// `__weavec_rt_cast` with the target's layout (its identity, size, name and
// parts), skipped for null; a local whose address becomes a `void *` is
// pushed on the thread's frame records for its scope; and in a build that
// does not pool, an allocation records its type with the runtime.
// RUN: %weavec_cc -O0 -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O0 -S -emit-llvm -mllvm -weavec-type-checks=0 %s -o - | FileCheck %s --check-prefix=OFF
#include <stdlib.h>
#include <weavec.h>

struct shape { int kind; };
struct circle { struct shape base; double radius; };

// CHECK-DAG: @[[CIRCLE:__weavec.layout.[0-9A-F]+]] = private constant { i64, i64, i64, i32, i32, [1 x { i64, i32, i32, i32, i32 }] } { i64 {{-?[0-9]+}}, i64 16, i64 sub {{.*}}, i32 1, i32 1, [1 x { i64, i32, i32, i32, i32 }] [{ i64, i32, i32, i32, i32 } { i64 {{-?[0-9]+}}, i32 0, i32 0, i32 1, i32 0 }] }
// CHECK-DAG: @[[TYPE:__weavec.type[.0-9]*]] = private global { i64, i64, ptr, i64 } { i64 {{-?[0-9]+}}, i64 0, ptr @[[CIRCLE]], i64 0 }

// CHECK-LABEL: define {{.*}} @radius{{(.weavec.safe)?}}(
// CHECK: %[[NULL:[0-9]+]] = icmp eq ptr %s, null
// CHECK: call void @__weavec_rt_cast(ptr %s, ptr @[[CIRCLE]], ptr @__weavec.site
// CHECK-NOT: __weavec.object
// CHECK: ret double
// OFF-LABEL: define {{.*}} @radius{{(.weavec.safe)?}}(
// OFF-NOT: __weavec_rt_cast
// OFF: ret double
WEAVEC_SAFE double radius(struct shape *s) { return ((struct circle *)s)->radius; }

// CHECK-LABEL: define {{.*}} @record{{(.weavec.safe)?}}(
// CHECK: %[[DEPTH:[0-9]+]] = call i32 @__weavec_rt_frame_push(ptr {{.*}}, ptr @[[CIRCLE]], i64 16)
// CHECK: call void @__weavec_rt_frame_pop(i32
// CHECK: ret
double peek(void *context);
WEAVEC_SAFE double record(void) {
  struct circle here = {.radius = 1};
  return peek(&here);
}

// CHECK-LABEL: define {{.*}} @make(
// CHECK: %[[BLOCK:.*]] = call ptr @calloc(
// CHECK-NEXT: call void @__weavec_rt_note_type(ptr %[[BLOCK]], ptr @[[TYPE]])
struct circle *make(void) { return calloc(1, sizeof(struct circle)); }
