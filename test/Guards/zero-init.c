// RFC 0035 §1: locals are zero-initialised in the enforcing modes.
// RUN: %weavec_cc -O0 %s -o %t
// RUN: %t | FileCheck %s
// RUN: %weavec_cc -O0 -fweavec-checks=none -S -emit-llvm %s -o - | FileCheck %s --check-prefix=NONE
#include <stdio.h>

__attribute__((noinline)) static int read(void) {
  int values[16];
  int sum = 0;
  for (int i = 0; i < 16; i++)
    sum |= values[i];
  return sum;
}

int main(void) {
  // CHECK: 0
  printf("%d\n", read());
  return 0;
}
// NONE-NOT: __weavec
