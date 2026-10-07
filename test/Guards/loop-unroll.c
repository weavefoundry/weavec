// RFC 0035 §6.4: a guarded loop with a small constant trip count is
// unrolled fully, as LLVM unrolls it without guards (a marker, not being
// `willreturn`, keeps LLVM's own full unrolling away), and its guards then
// merge.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s
#include <stdint.h>

// CHECK-LABEL: define {{.*}}@carry(
// CHECK-NOT: phi
// CHECK-NOT: __weavec_rt_range_ok
// CHECK: ret void
void carry(uint32_t *d, const uint32_t *t) {
  uint32_t cc = 0;
  for (int i = 0; i < 9; i++) {
    uint32_t z = t[i] + cc;
    d[i] = z & 0x3FFFFFFF;
    cc = z >> 30;
  }
}
