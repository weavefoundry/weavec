// RFC 0035 §2.4: an index from a pointer that may be null is tested: a null
// base with a large index would reach mapped memory.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <stdlib.h>

char *(*volatile source)(void);
static char *none(void) { return NULL; }

int main(int argc, char **argv) {
  (void)argv;
  source = none;
  char *p = source();
  long i = 0x100000000L * argc;
  // CHECK: weavec: null-dereference at {{.*}}null-base.c:[[@LINE+1]]:{{[0-9]+}}: access at 0x100000000
  return p[i];
}
