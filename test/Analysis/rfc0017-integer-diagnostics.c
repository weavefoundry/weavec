// RFC 0017: definite invalid C integer operations have a stable diagnostic.
// RUN: not %weavec %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: %weavec -Wno-error=weavec-invalid-integer-operation %s -- 2>&1 | FileCheck %s --check-prefix=LOWERED
// LOWERED-COUNT-6: warning: invalid integer operation:
#include "../Inputs/prelude.h"
int add(int n) {
  if (n != 2147483647) return 0;
  // CHECK: rfc0017-integer-diagnostics.c:[[@LINE+1]]:10: error: invalid integer operation: signed integer overflow [weavec::invalid-integer-operation]
  return n + 1;
}
int division(int n) {
  if (n != 0) return 0;
  // CHECK: error: invalid integer operation: division by zero [weavec::invalid-integer-operation]
  return 2 / n;
}
int min_divided(int n) {
  if (n != (-2147483647 - 1)) return 0;
  // CHECK: error: invalid integer operation: signed division overflow [weavec::invalid-integer-operation]
  return n / -1;
}
int shift(int n) {
  if (n != 32) return 0;
  // CHECK: error: invalid integer operation: invalid shift count [weavec::invalid-integer-operation]
  return 1 << n;
}
int negative_shift(int n) {
  if (n != -1) return 0;
  // CHECK: error: invalid integer operation: invalid signed left shift [weavec::invalid-integer-operation]
  return n << 1;
}
void dimension(int n) {
  if (n != 0) return;
  // CHECK: error: invalid integer operation: nonpositive variable array dimension [weavec::invalid-integer-operation]
  char a[n]; (void)a;
}
// CHECK: 6 errors generated.
