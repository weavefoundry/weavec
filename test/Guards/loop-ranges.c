// RFC 0035 §6.4: a loop whose trip count is known when it is entered runs
// without the guards of its affine accesses when their whole ranges are
// addressable, and as the guarded loop otherwise, which stops at the access
// that fails, short trip or long.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 64 64
// RUN: not --crash %t 64 65 2>&1 | FileCheck %s --check-prefix=LONG
// RUN: not --crash %t 4 5 2>&1 | FileCheck %s --check-prefix=SHORT
#include <stdlib.h>

// IR-LABEL: define {{.*}}@sum(
// IR: call i32 @__weavec_rt_range_ok(
// IR: <{{[0-9]+}} x i32>
__attribute__((noinline)) long sum(const int *p, int n) {
  long s = 0;
  for (int i = 0; i < n; i++)
    // LONG: weavec: heap-buffer-overflow at {{.*}}loop-ranges.c:[[@LINE+2]]:{{[0-9]+}}: read of 4 bytes
    // SHORT: weavec: heap-buffer-overflow at {{.*}}loop-ranges.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
    s += p[i];
  return s;
}

int main(int argc, char **argv) {
  int size = argc > 2 ? atoi(argv[1]) : 0;
  int count = argc > 2 ? atoi(argv[2]) : 0;
  int *p = calloc((size_t)size, sizeof *p);
  return (int)(sum(p, count) & 1);
}
