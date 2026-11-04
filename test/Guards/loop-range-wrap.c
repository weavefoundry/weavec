// RFC 0037 §4: a loop range whose bounds overflow 64 bits is not
// addressable, so the guarded loop runs: an allocation whose size wrapped
// (n * 8 for n = 2^61 + 1 is 8) and a `do ... while` loop entered with a
// count of 0 (its trip count wraps) stop at their first bad access.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t fill 4
// RUN: not --crash %t fill 0x2000000000000001 2>&1 | FileCheck %s --check-prefix=FILL
// RUN: not --crash %t copy 0 2>&1 | FileCheck %s --check-prefix=COPY
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// (The optimizer folds the multiplication's overflow test into a compare.)
// IR-LABEL: define {{.*}}@fill(
// IR: call { i64, i1 } @llvm.uadd.with.overflow.i64(
// IR: call i32 @__weavec_rt_range_ok(
__attribute__((noinline)) void fill(uint64_t *a, uint64_t n) {
  for (uint64_t i = 0; i < n; i++)
    // FILL: weavec: heap-buffer-overflow at {{.*}}loop-range-wrap.c:[[@LINE+1]]:{{[0-9]+}}: write of 8 bytes
    a[i] = 0x4141414141414141ULL;
}

__attribute__((noinline)) void copy(char *d, const char *s, size_t n) {
  do {
    // COPY: weavec: heap-buffer-overflow at {{.*}}loop-range-wrap.c:[[@LINE+1]]:{{[0-9]+}}: {{read|write}} of 1 bytes
    *d++ = *s++;
  } while (--n);
}

int main(int argc, char **argv) {
  const uint64_t n = strtoull(argv[2], NULL, 0);
  if (strcmp(argv[1], "fill") == 0) {
    uint64_t *a = malloc((size_t)(n * 8));
    fill(a, n);
    free(a);
  } else {
    char *d = calloc(32, 1);
    char *s = calloc(32, 1);
    copy(d, s, (size_t)n);
  }
  return 0;
}
