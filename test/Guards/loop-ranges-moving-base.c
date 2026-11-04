// RFC 0035 §6.4, RFC 0037 §8.5: a loop whose accesses are computed from a
// pointer the loop moves is versioned when colors are off; with colors
// its root's color cannot be tested once before the loop, and it runs as
// the guarded loop. Either way an access past the block stops.
// RUN: %weavec_cc -O2 -fno-weavec-colors -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %weavec_cc -O2 -fno-weavec-colors %s -o %t
// RUN: %t 30 10
// RUN: not --crash %t 30 11 2>&1 | FileCheck %s --check-prefix=PAST
// RUN: %weavec_cc -O2 %s -o %t.colors
// RUN: %t.colors 30 10
// RUN: not --crash %t.colors 30 11 2>&1 | FileCheck %s --check-prefix=PAST
#include <stdlib.h>

// IR-LABEL: define {{.*}}@walk(
// IR: call i32 @__weavec_rt_range_ok(
__attribute__((noinline)) long walk(const int *p, long n) {
  long t = 0;
  for (long i = 0; i < n; i++, p += 3)
    // PAST: weavec: heap-buffer-overflow at {{.*}}loop-ranges-moving-base.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
    t += p[1] + p[2];
  return t;
}

int main(int argc, char **argv) {
  const long size = argc > 2 ? atol(argv[1]) : 0;
  const long count = argc > 2 ? atol(argv[2]) : 0;
  int *p = calloc((size_t)size, sizeof *p);
  return (int)(walk(p, count) & 1);
}
