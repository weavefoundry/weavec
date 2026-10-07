// RFC 0035 §3.3: a lifetime end poisons its local; a use through a pointer
// that outlived the scope traps (lifetime markers exist when optimising).
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <stdio.h>

int *escaped;

int main(void) {
  {
    int x = 1;
    escaped = &x;
  }
  // CHECK: weavec: stack-use-after-scope at {{.*}}use-after-scope.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  printf("%d\n", *escaped);
  return 0;
}
