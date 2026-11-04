// RFC 0035 §2.3: a guard is an inline check of the shadow: the descriptor's
// base and mask loaded once per function, then a shift, a mask, a load and
// a branch per access, with the runtime's slow path behind it. RFC 0037
// §7–§8: a whole granule is any byte below 0x80 (its sign bit is clear);
// an access computed from a pointer compares its granule's byte with the
// pointer's, and a mismatch goes to the runtime's color check.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

// CHECK-LABEL: define {{.*}}@load(
// CHECK: load ptr, ptr @__weavec_rt_shadow
// CHECK: load i64, ptr getelementptr {{.*}}@__weavec_rt_shadow
// CHECK: lshr i64 %{{.*}}, 4
// CHECK: and i64
// CHECK: load i8, ptr
// CHECK: icmp slt i8 %{{.*}}, 0
// CHECK: call {{.*}}void @__weavec_rt_guard(i64 %{{.*}}, i64 4, ptr @__weavec.site
// CHECK-NOT: @__weavec_rt_jump
// CHECK: load i32, ptr
int load(const int *p) { return *p; }

// CHECK-LABEL: define {{.*}}@get(
// The root's shadow byte, then the access's, compared; poison, a partial
// granule's end and another color share one cold call.
// CHECK: load i8, ptr
// CHECK: sext i8 %{{.*}} to i32
// CHECK: load i8, ptr
// CHECK: icmp ne i32
// CHECK: call {{.*}}void @__weavec_rt_guard_jump(i64 %{{.*}}, i64 4, i64 %{{.*}}, i64 %{{.*}}, ptr @__weavec.site
// CHECK: load i32, ptr
int get(const int *p, long i) { return p[i]; }

// No marker survives to the object.
// CHECK-NOT: @__weavec.guard(
