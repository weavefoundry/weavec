// RFC 0051 §2, §3: in a safe function, signed arithmetic wraps (no `nsw`),
// a shift's count is taken modulo the width (any width), a division is not
// `exact`, a copy is a move, a conversion from floating
// point saturates, nothing is assumed, loops may run forever, a division
// that may divide by zero (or the least value by -1) stops, the end of a
// function that must return a value stops, and a large frame is probed. A
// guarded function of the same unit keeps what Clang emits, and so does a
// unit with no safe function.
// RUN: %weavec_cc -O0 -S -emit-llvm -Wno-return-type %s -o - | FileCheck %s
// RUN: %weavec_cc -O0 -S -emit-llvm -Wno-return-type -DNOSAFE %s -o - | FileCheck %s --check-prefix=NONE
#include <weavec.h>

#ifndef NOSAFE
#define DEFINED WEAVEC_SAFE
#else
#define DEFINED
#endif

// CHECK-LABEL: define {{.*}}@add(
// CHECK-NOT: nsw
// CHECK: mul i32
// CHECK: add i32
// CHECK: ret
DEFINED int add(int a, int b) { return a + b * 3; }

// CHECK-LABEL: define {{.*}}@shift(
// CHECK: and i32 %{{.*}}, 31
// CHECK: shl i32
DEFINED int shift(int a, int b) { return a << b; }

// CHECK-LABEL: define {{.*}}@convert(
// CHECK: call i32 @llvm.fptosi.sat.i32.f64(
DEFINED int convert(double d) { return (int)d; }

// CHECK-LABEL: define {{.*}}@quotient(
// CHECK: icmp eq i32 %{{.*}}, 0
// CHECK: call void @__weavec_rt_division(ptr @__weavec.site{{.*}}, i32 0)
// CHECK: icmp eq i32 %{{.*}}, -2147483648
// CHECK: icmp eq i32 %{{.*}}, -1
// CHECK: call void @__weavec_rt_division(ptr @__weavec.site{{.*}}, i32 32)
// CHECK: sdiv i32
DEFINED int quotient(int a, int b) { return a / b; }

// A constant divisor other than 0 and -1 needs no check.
// CHECK-LABEL: define {{.*}}@half(
// CHECK-NOT: __weavec_rt_division
// CHECK: ret
DEFINED int half(int a) { return a / 2; }

// CHECK-LABEL: define {{.*}}@hinted(
// CHECK: call void @__weavec_rt_unreachable(
DEFINED int hinted(int x) {
  if (x < 0)
    __builtin_unreachable();
  return x;
}

// CHECK-LABEL: define {{.*}}@sign(
// CHECK: call void @__weavec_rt_missing_return(
DEFINED int sign(int x) {
  if (x > 0)
    return 1;
  if (x < 0)
    return -1;
}

// A width that is not a power of two takes the count's remainder.
// CHECK-LABEL: define {{.*}}@wide(
// CHECK: urem i24 %{{.*}}, 24
// CHECK: lshr i24
DEFINED unsigned wide(unsigned _BitInt(24) a, unsigned b) {
  return (unsigned)(a >> b);
}

// A pointer difference is the quotient, whatever the remainder: no
// `exact`.
// CHECK-LABEL: define {{.*}}@distance(
// CHECK-NOT: exact
// CHECK: sdiv i64 %{{.*}}, 12
struct triple {
  int a, b, c;
};
DEFINED long distance(int i, int j) {
  struct triple xs[4] = {0};
  return &xs[i & 3] - &xs[j & 3];
}

// `__builtin_memcpy_inline` of overlapping bytes is a move too.
// CHECK-LABEL: define {{.*}}@slide(
// CHECK-NOT: memcpy.inline
// CHECK: call void @llvm.memmove.p0.p0.i64(
DEFINED int slide(void) {
  char bytes[32] = {0};
  __builtin_memcpy_inline(&bytes[1], &bytes[0], 16);
  return bytes[1];
}

// A guarded function keeps Clang's arithmetic.
// CHECK-LABEL: define {{.*}}@guarded(
// CHECK: add nsw i32
WEAVEC_GUARDED int guarded(int a, int b) { return a + b; }
// Safe functions' frames are probed page by page.
// CHECK: attributes #{{[0-9]+}} = { {{.*}}"probe-stack"="inline-asm"

// NONE-LABEL: define {{.*}}@add(
// NONE: add nsw i32
// NONE-LABEL: define {{.*}}@quotient(
// NONE-NOT: __weavec_rt_division
// NONE: sdiv i32
