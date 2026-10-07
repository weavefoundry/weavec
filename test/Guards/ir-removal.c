// RFC 0035 §6: what local rules remove. A local indexed in bounds needs no
// guard (6.1); the fields one block reads through one pointer share one
// inline check of their hull (6.3).
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

struct pair {
  int a;
  int b;
  long c;
};

// CHECK-LABEL: define {{.*}}@local(
// CHECK-NOT: __weavec_rt_guard
// CHECK: ret i32
int local(unsigned i) {
  int a[4] = {1, 2, 3, 4};
  return a[i & 3];
}

// One inline check of the 16 bytes; when it fails, each field is checked on
// its own so that the report names the access that fails.
// CHECK-LABEL: define {{.*}}@fields(
// CHECK: call {{.*}}void @__weavec_rt_guard(i64 %{{.*}}, i64 4,
// CHECK: call {{.*}}void @__weavec_rt_guard(i64 %{{.*}}, i64 4,
// CHECK: call {{.*}}void @__weavec_rt_guard(i64 %{{.*}}, i64 8,
// CHECK-NOT: call {{.*}}void @__weavec_rt_guard
// CHECK: ret i64
long fields(const struct pair *p) { return p->a + p->b + p->c; }
