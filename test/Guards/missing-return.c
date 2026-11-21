// RFC 0051 §3.1 in C: a safe function that ends without returning its
// value, whose caller uses it, stops (`missing-return`).
// RUN: %weavec_cc -O2 -fweavec-safe -Wno-return-type %s -o %t
// RUN: %t 0 | FileCheck %s --check-prefix=OK
// RUN: not --crash %t 5 2>&1 | FileCheck %s
#include <stdio.h>
#include <stdlib.h>
#include <weavec.h>

enum op { ADD, SUB };

__attribute__((noinline)) long step(enum op op, long a, long b) {
  switch (op) {
  case ADD:
    return a + b;
  case SUB:
    return a - b;
  }
  // CHECK: weavec: missing-return at {{.*}}missing-return.c:[[@LINE+1]]:1: the function ended without returning a value
}

WEAVEC_GUARDED int main(int argc, char **argv) {
  const int op = argc > 1 ? atoi(argv[1]) : 0;
  // OK: 5
  printf("%ld\n", step((enum op)op, 2, 3));
  return 0;
}
