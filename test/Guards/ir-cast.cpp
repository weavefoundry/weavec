// RFC 0054 §2.2: in safe C++ a downcast to a class with virtual functions
// compares the type information the object's vtable names with the
// class's, and calls the C++ ABI's dynamic cast only when they differ
// (`__weavec_rt_cast_fail` when its result is not the object); a downcast
// to any other class, and a `static_cast` from `void *`, call
// `__weavec_rt_cast`. In the fast build the checked object's accesses at
// constant offsets are believed.
// RUN: %weavec_cxx -std=c++20 -O1 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s
#include <weavec.h>

struct Shape { int kind = 0; virtual ~Shape() = default; };
struct Circle final : Shape { double r = 1; };
struct Plain { int kind = 0; };
struct Wide : Plain { long a = 0; };

// CHECK-LABEL: define {{.*}} @_Z6radiusRK5Shape(
// CHECK: %[[TABLE:.*]] = load ptr, ptr %shape, align 8
// CHECK: %[[SLOT:.*]] = getelementptr i8, ptr %[[TABLE]], i64 -8
// CHECK: %[[INFO:.*]] = load ptr, ptr %[[SLOT]], align 8
// CHECK: getelementptr i8, ptr %[[TABLE]], i64 -16
// CHECK: icmp ne ptr %[[INFO]], @_ZTI6Circle
// CHECK: call ptr @__dynamic_cast(ptr {{.*}}, ptr nonnull @_ZTI5Shape, ptr nonnull @_ZTI6Circle, i64 -1)
// CHECK: call void @__weavec_rt_cast_fail(
// CHECK-NOT: __weavec_rt_guard
// CHECK: ret double
double radius(const Shape &shape) { return static_cast<const Circle &>(shape).r; }

// CHECK-LABEL: define {{.*}} @_Z4wideP5Plain(
// CHECK: call void @__weavec_rt_cast(ptr {{.*}}, ptr nonnull @__weavec.layout.{{[0-9A-F]+}}, ptr nonnull @__weavec.site
long wide(Plain *p) { return static_cast<Wide *>(p)->a; }

// CHECK-LABEL: define {{.*}} @_Z5ticksPv(
// CHECK: call void @__weavec_rt_cast(
long ticks(void *context) { return static_cast<Wide *>(context)->a; }
