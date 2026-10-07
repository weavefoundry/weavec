// RFC 0035 §2.3: a guard is an inline check of the shadow: the descriptor's
// base and mask loaded once per function, then a shift, a mask, a load and
// a branch per access, with the runtime's slow path behind it.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

// CHECK-LABEL: define {{.*}}@get(
// CHECK: load ptr, ptr @__weavec_rt_shadow
// CHECK: load i64, ptr getelementptr {{.*}}@__weavec_rt_shadow
// CHECK: lshr i64 %{{.*}}, 4
// CHECK: and i64
// CHECK: load i8, ptr
// CHECK: icmp ne i8
// CHECK: call {{.*}}void @__weavec_rt_guard(i64 %{{.*}}, i64 4, ptr @__weavec.site
// CHECK: load i32, ptr
int get(const int *p, long i) { return p[i]; }

// No marker survives to the object.
// CHECK-NOT: @__weavec.guard(
