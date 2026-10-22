// RFC 0035 §3.4: a dynamic alloca at the very top of a function (libsodium's
// sodium_stackzero) gets its redzone, at every optimisation level.
// RUN: %weavec_cc -O0 %s -o %t0 && %t0 64
// RUN: %weavec_cc -O2 %s -o %t2 && %t2 64
// RUN: not --crash %t2 0 2>&1 | FileCheck %s
#include <stdlib.h>
#include <string.h>

__attribute__((noinline)) void zero(void *p, size_t n, size_t at) {
  memset(p, 0, n);
  // CHECK: weavec: {{(dynamic-)?}}stack-buffer-overflow at {{.*}}alloca-entry.c:[[@LINE+1]]:{{[0-9]+}}: write of 1 bytes
  ((char *)p)[at] = 1;
}

void stackzero(size_t len, size_t at) { zero(__builtin_alloca(len), len, at); }

int main(int argc, char **argv) {
  // An index inside the alloca, or (0) one past its 64 bytes.
  size_t at = (size_t)atol(argv[argc - 1]);
  stackzero(64, at == 0 ? 64 : at - 1);
  return 0;
}
