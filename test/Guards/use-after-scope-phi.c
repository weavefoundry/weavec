// RFC 0035 §3.2: a local reached only through a phi (the pointer that
// outlived its scope is null on the other path) is still in the frame, so
// the use after its scope traps at every optimisation level.
// RUN: %weavec_cc -O0 %s -o %t0
// RUN: not --crash %t0 2>&1 | FileCheck %s
// RUN: %weavec_cc -O2 %s -o %t2
// RUN: not --crash %t2 2>&1 | FileCheck %s
#include <stdio.h>

int main(int argc, char **argv) {
  const int *p = NULL;
  (void)argv;
  if (argc > 0) {
    int tmp[4];
    for (int i = 0; i < 4; i++)
      tmp[i] = i + argc;
    p = tmp;
  }
  // CHECK: weavec: stack-use-after-scope at {{.*}}use-after-scope-phi.c:[[@LINE+1]]:{{[0-9]+}}: read of 4 bytes
  printf("%d\n", p[argc & 3]);
  return 0;
}
