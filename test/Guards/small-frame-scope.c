// RFC 0036 amendment 8: locals share bytes only in a frame larger than 512
// bytes without sharing. Three 96-byte locals in disjoint scopes take 448
// bytes apart (a 64-byte left zone and 128 bytes each), so each keeps its
// own, and a use after the first one's scope, while the third is live, is
// still a stack-use-after-scope.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <stdio.h>

static volatile char *stale;

__attribute__((noinline)) void keep(volatile char *p) {
  if (stale == 0)
    stale = p;
}

// IR-LABEL: define {{.*}} @scopes(
// IR: alloca [448 x i8], align 32
// IR-NOT: alloca [
// IR: ret
__attribute__((noinline)) int scopes(void) {
  int sum = 0;
  { volatile char a[96]; a[0] = 1; keep(a); sum += a[0]; }
  { volatile char b[96]; b[0] = 2; keep(b); sum += b[0]; }
  {
    volatile char c[96];
    c[0] = 3;
    keep(c);
    // CHECK: weavec: stack-use-after-scope at {{.*}}small-frame-scope.c:[[@LINE+1]]:{{[0-9]+}}: read of 1 bytes
    sum += stale[0] + c[0];
  }
  return sum;
}

int main(void) {
  printf("%d\n", scopes());
  return 0;
}
