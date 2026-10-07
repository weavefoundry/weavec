// RFC 0035 §3.2: a local whose address escapes is placed in the function's
// frame with redzones written at entry and cleared at each return.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

void use(char *);

// CHECK-LABEL: define {{.*}}@escapes(
// CHECK: alloca [128 x i8], align 32
// At entry: the left redzone (four granules of 0xF1), the object out of
// scope (0xF8) and the right redzone (0xF3), little-endian in one store. Its lifetime
// start makes its 10 bytes addressable (a partial granule of 10), its end
// puts it out of scope again, and the return clears the frame.
// CHECK: store i64 -868082052615769615, ptr
// CHECK: store i8 10, ptr
// CHECK: call void @use(
// CHECK: store i8 -8, ptr
// CHECK: store i64 0, ptr
// CHECK: ret void
void escapes(void) {
  char buf[10];
  use(buf);
}

// CHECK-LABEL: define {{.*}}@stays(
// CHECK-NOT: alloca [{{[0-9]+}} x i8], align 32
// CHECK-NOT: __weavec_rt_overlap
// CHECK: ret i32
int stays(int i) {
  int a[2] = {1, 2};
  return a[i & 1];
}
