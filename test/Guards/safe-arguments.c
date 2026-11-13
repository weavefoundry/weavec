// RFC 0042 §6: a safe call's arguments are matched with the callee's
// parameters by their source position: a struct returned through a hidden
// pointer is no parameter; a null `_OR_NULL` argument and an empty range
// pass the caller's check, and an overstated count still stops.
// RUN: %weavec_cc -O0 -g -fweavec-safe %s -o %t0
// RUN: %t0 | FileCheck %s
// RUN: not --crash %t0 over 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: %weavec_cc -O2 -g -fweavec-safe %s -o %t2
// RUN: %t2 | FileCheck %s
// RUN: not --crash %t2 over 2>&1 | FileCheck %s --check-prefix=OVER
// RUN: %weavec_cc -O2 -g -fweavec-safe -fweavec-checks=verify %s -o %tv
// RUN: %tv | FileCheck %s
#include <weavec.h>
#include <stdio.h>
#include <string.h>

typedef struct { size_t a, b, c; } Triple;

static __attribute__((noinline)) Triple triple(const int *p) {
  Triple t;
  t.a = (size_t)*p;
  t.b = 2;
  t.c = 3;
  return t;
}

size_t sumTriple(const int *p) {
  Triple t;
  t = triple(p);
  return t.a + t.b + t.c;
}

static __attribute__((noinline)) int count(
    const char *WEAVEC_COUNTED_BY_OR_NULL(n) b, size_t n) {
  int s = 0;
  if (b == NULL)
    return -1;
  for (size_t i = 0; i < n; i++)
    s += b[i];
  return s;
}

static __attribute__((noinline)) int page(
    const char *WEAVEC_COUNTED_BY_OR_NULL(64) b) {
  return b ? b[0] : -2;
}

int nulls(void) { return count(NULL, 0) + count(NULL, 5) + page(NULL); }

// OVER: weavec: pointer-out-of-bounds at {{.*}}safe-arguments.c:[[@LINE+1]]:{{[0-9]+}}: read of 5 bytes
int counted(size_t k) { char buf[4] = {1, 2, 3, 4}; return count(buf, k); }

int atEnd(void) { char buf[4] = {1, 2, 3, 4}; return count(buf + 4, 0); }

WEAVEC_GUARDED int main(int argc, char **argv) {
  if (argc > 1 && strcmp(argv[1], "over") == 0)
    return counted(5);
  int x = 1;
  // CHECK: 6 -4 10 0
  printf("%zu %d %d %d\n", sumTriple(&x), nulls(), counted(4), atEnd());
  return 0;
}
