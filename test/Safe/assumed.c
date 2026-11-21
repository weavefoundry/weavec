// RFC 0051 §4.1 in C: `restrict` on a parameter or a local, the assume
// builtins and the allocation attributes that assert what nothing checks.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -w -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <stddef.h>

long sum2(long *restrict a, long *restrict b) { // expected-error 2 {{'restrict' tells the optimizer what nothing checks}}
  *a += 1;
  *b += 1;
  return *a + *b;
}

long local(long *p) {
  long *restrict q = p; // expected-error {{'restrict' tells the optimizer what nothing checks}}
  return *q;
}

long hinted(long x) {
  __builtin_assume(x > 0); // expected-error {{'__builtin_assume' tells the optimizer what nothing checks}}
  return x;
}

__attribute__((const)) long square(long x); // expected-error {{'const' tells the optimizer what nothing checks}}
long squares(long x) { return square(x) + square(x + 1); }
long square(long x) { return x * x; }

// Accepted: a pointer without a promise.
long plain(long *a, long *b) { return *a + *b; }
