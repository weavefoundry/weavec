// RFC 0034 §4: a registered global owns the granules it starts in. The
// guard passes pad it to whole granules, and by one more when it fills its
// last, so that nothing the linker places after it (a string literal, an
// unregistered global) shares its last granule or starts at its one-past
// address, where the shadow would give those bytes to it.
//
// RUN: %weavec_cc -O0 -S -emit-llvm %s -o - | FileCheck %s
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s

// CHECK-DAG: @eight = internal global <{ [8 x i8], [8 x i8] }> <{ [8 x i8] c"\03{{.*}}", [8 x i8] zeroinitializer }>, align 16
// CHECK-DAG: @whole = {{(dso_local )?}}constant <{ [64 x i8], [16 x i8] }> {{.*}}, align 16
// CHECK-DAG: @odd = {{(dso_local )?}}global <{ [3 x i32], [4 x i8] }> {{.*}}, align 16
// The descriptors still register the object's own size.
// CHECK-DAG: ptr @eight, ptr inttoptr (i64 8 to ptr)
// CHECK-DAG: ptr @whole, ptr inttoptr (i64 64 to ptr)
// CHECK-DAG: ptr @odd, ptr inttoptr (i64 12 to ptr)

static char eight[8] = {3};
const char whole[64] = "0123456789abcdef0123456789abcdef0123456789abcdef012345678";
int odd[3] = {1, 2, 3};

__attribute__((noinline)) static int sum(const char *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

int main(int argc, char **argv) {
  (void)argv;
  eight[argc & 7] = 1;
  return sum(eight, 8) + sum(whole, 64) + odd[argc % 3];
}
