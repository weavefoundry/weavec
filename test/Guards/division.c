// RFC 0051 §3.2: in a safe function a division by zero stops
// (`division-by-zero`), and so does a division of the least value by -1
// (`division-overflow`), on every target: arm64 computes both without a
// trap. A shift by the width or more takes its count modulo the width,
// and signed overflow wraps.
// RUN: %weavec_cc -O2 -fweavec-safe %s -o %t
// RUN: %t 1 1 | FileCheck %s --check-prefix=OK
// RUN: not --crash %t 1 0 2>&1 | FileCheck %s --check-prefix=ZERO
// RUN: not --crash %t -2147483648 -1 2>&1 | FileCheck %s --check-prefix=LEAST
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

__attribute__((noinline)) int quotient(int a, int b) {
  // ZERO: weavec: division-by-zero at {{.*}}division.c:[[@LINE+2]]:{{[0-9]+}}: division by zero
  // LEAST: weavec: division-overflow at {{.*}}division.c:[[@LINE+1]]:{{[0-9]+}}: the least 32-bit integer divided by -1
  return a / b;
}

__attribute__((noinline)) int shift(int a, int b) { return a << b; }
__attribute__((noinline)) int grows(int a) { return a + 1 > a; }
__attribute__((noinline)) int convert(double d) { return (int)d; }

WEAVEC_GUARDED int main(int argc, char **argv) {
  const int a = argc > 2 ? atoi(argv[1]) : 1;
  const int b = argc > 2 ? atoi(argv[2]) : 1;
  // OK: quotient 1 shift 256 grows 0 convert 2147483647
  printf("quotient %d shift %d grows %d convert %d\n", quotient(a, b),
         shift(1, 39 + b), grows(INT_MAX - b + 1), convert(1e30 * b));
  return 0;
}
