// RFC 0034 §4 (amendment 19): a function enters its objects loose (flag 1)
// when a function it calls may be inlined into it and has automatic storage
// the plan does not enter: inlined, that storage lies in the caller's frame,
// possibly in the last granule of one of its objects. A `noinline` callee's
// storage stays in its own frame.
//
// RUN: %weavec_cc -O0 -Xclang -disable-llvm-passes -S -emit-llvm %s -o - | FileCheck %s

static __attribute__((noinline)) long rd(const void *p) {
  return *(const long *)p;
}
static long literal(long v) { return rd((long[1]){v}); }
static long middle(long v) { return literal(v); }
static __attribute__((noinline)) long apart(long v) { return rd((long[1]){v}); }

// CHECK-LABEL: define {{.*}}i64 @inlines(
// CHECK: call ptr @__weavec_stack_enter(ptr noundef %{{[a-z0-9.]+}}, i64 noundef 8, i32 noundef 1)
long inlines(long v) {
  long a = v;
  return middle(v) + rd(&a);
}

// CHECK-LABEL: define {{.*}}i64 @calls_apart(
// CHECK: call ptr @__weavec_stack_enter(ptr noundef %{{[a-z0-9.]+}}, i64 noundef 8, i32 noundef 0)
long calls_apart(long v) {
  long a = v;
  return apart(v) + rd(&a);
}
