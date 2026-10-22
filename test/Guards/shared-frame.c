// RFC 0036 §2: locals whose lifetimes never overlap (the arms of a switch)
// share the frame's bytes, each with its redzone; an overflow of one of them
// is still a stack-buffer-overflow.
// RUN: %weavec_cc -O2 -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t 1 7
// RUN: not --crash %t 1 200 2>&1 | FileCheck %s
#include <stdlib.h>

__attribute__((noinline)) void put(char *p, int i) {
  // CHECK: weavec: stack-buffer-overflow at {{.*}}shared-frame.c:[[@LINE+1]]:{{[0-9]+}}: write of 1 bytes
  p[i] = 1;
}

// A left zone of 64 bytes and one 200-byte object with its redzone, not
// four.
// IR-LABEL: define {{.*}} @pick(
// IR: alloca [320 x i8], align 32
// IR-NOT: alloca [
// IR: ret
__attribute__((noinline)) int pick(int kind, int i) {
  switch (kind) {
  case 0: { char a[200]; put(a, i); return a[i & 7]; }
  case 1: { char b[200]; put(b, i); return b[i & 7]; }
  case 2: { char c[200]; put(c, i); return c[i & 7]; }
  default: { char d[200]; put(d, i); return d[i & 7]; }
  }
}

int main(int argc, char **argv) {
  (void)argc;
  return pick(atoi(argv[1]), atoi(argv[2])) == 1 ? 0 : 1;
}
