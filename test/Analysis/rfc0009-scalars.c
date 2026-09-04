// RFC 0009, *Scalar facts in the state* and *Refuting guards in the state*:
// a test of an integer place refines what is known about it, a move or a
// held resource made under a fact carries it as a guard, and a later test
// that contradicts the guard drops the record.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

struct buf {
  char *data;
  int owned;
};

// Clean: two tests of one integer are one test.

// DUMP-LABEL: function 'truthy':
// DUMP: exit: moved{p@[[@LINE+4]]:5 freed(free) when[c positive|negative]}
// DUMP-NEXT: summary: p: freed(free) when[c positive|negative]; *p: read; stores{} returns{}
void truthy(int c, char *p) {
  if (c)
    free(p);
  if (!c)
    use(p);
}

// DUMP-LABEL: function 'eqzero':
// DUMP: exit: moved{p@[[@LINE+4]]:5 freed(free) when[n =0]}
// DUMP-NEXT: summary: p: freed(free) when[n =0]; *p: read; stores{} returns{}
void eqzero(int n, char *p) {
  if (n == 0)
    free(p);
  if (n != 0)
    use(p);
}

// DUMP-LABEL: function 'sign':
// DUMP: exit: moved{p@[[@LINE+3]]:5 freed(free) when[n positive]}
void sign(int n, char *p) {
  if (n > 0)
    free(p);
  if (n <= 0)
    use(p);
}

// DUMP-LABEL: function 'constant':
// DUMP: exit: moved{p@[[@LINE+3]]:5 freed(free) when[n =3]}
void constant(int n, char *p) {
  if (n == 3)
    free(p);
  if (n == 4)
    use(p);
}

// DUMP-LABEL: function 'switched':
// DUMP: exit: moved{p@[[@LINE+4]]:5 freed(free) when[n =0]}
void switched(int n, char *p) {
  switch (n) {
  case 0:
    free(p);
    break;
  default:
    break;
  }
  switch (n) {
  case 1:
    use(p);
    break;
  default:
    break;
  }
}

// A local assigned a constant: the `if (c)` edge is infeasible and the move
// never happens. The summary says nothing about `c` (it is not the caller's).
// DUMP-LABEL: function 'local_constant':
// DUMP: exit: moved{p@[[@LINE+7]]:3 freed(free)}
// DUMP-NEXT: summary: p: freed(free); *p: read; stores{} returns{}
void local_constant(char *p) {
  int c = 0;
  if (c)
    free(p);
  use(p);
  free(p);
}

// A field of the caller's object.
// DUMP-LABEL: function 'field':
// DUMP: summary: b->data: read|freed(free) when[b->owned positive|negative]; *b->data: read; b->owned: read; stores{} returns{} requires{b}
void field(struct buf *b) {
  if (b->owned)
    free(b->data);
  if (!b->owned)
    use(b->data);
}

// A held resource under a guard the early return's edge refutes: no leak.
int merged(int c) {
  char *p = NULL;
  if (c)
    p = malloc(8);
  if (!c)
    return -1;
  free(p);
  return 0;
}

// Reported: the tests do not exclude each other.
void overlap(int n, char *p) {
  if (n > 0)
    free(p);
  if (n != 0)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: error: use of 'p' after it was freed [weavec::use-after-free]
    use(p);
}

// Reported: the flag was reassigned from an unknown value, which drops the
// guard's conjunct and leaves the move unconditional.
void reassigned(int c, int d, char *p) {
  if (c)
    free(p);
  c = d;
  if (!c)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: error: use of 'p' after it was freed [weavec::use-after-free]
    use(p);
}

// Reported: a computed expression establishes nothing about `n`.
void computed(int n, char *p) {
  if (n == 0)
    free(p);
  if ((n & 1) != 0)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: error: use of 'p' after it was freed [weavec::use-after-free]
    use(p);
}

// Reported: a guarded resource whose guard nothing refutes is still lost.
int leaked(int c) {
  char *p = NULL;
  if (c)
    p = malloc(8);
  // CHECK: rfc0009-scalars.c:[[@LINE+1]]:3: warning: 'p' is leaked [weavec::leak]
  return 0;
}

// A class is that of the mathematical value and a comparison is decided in
// its operands' type (RFC 0009, *Assumptions*): `ULONG_MAX` is not `-1`, so
// the edge on which `i > ULONG_MAX` fails stays live, and `(size_t)-1` is a
// constant the checker does not hold, so a test against it decides nothing.
static void touch(char *p) { p[0] = 1; }

int above_max(void) {
  size_t i = 0;
  char *p = malloc(4);
  if (i > 18446744073709551615UL) { free(p); return 1; }
  // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: error: 'p', which may be null, is passed to 'touch', which dereferences it [weavec::null-dereference]
  touch(p);
  free(p);
  return 0;
}

int sentinel(void) {
  size_t n = (size_t)-1;
  char *p = malloc(4);
  if (n == (size_t)-1) { free(p); return 1; }
  // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: error: 'p', which may be null, is passed to 'touch', which dereferences it [weavec::null-dereference]
  touch(p);
  free(p);
  return 0;
}

// `unsigned x = -1` is `UINT_MAX`, a positive value, so `x > 5` holds; a
// negative signed operand of an unsigned comparison ranks above every
// non-negative one.
void minus_one(void) {
  unsigned x = -1;
  int y = -1;
  char *p = malloc(4);
  if (x > 5)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:11: error: 'p', which may be null, is passed to 'touch', which dereferences it [weavec::null-dereference]
    touch(p);
  free(p);
  p = malloc(4);
  if (y > 5u)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:11: error: 'p', which may be null, is passed to 'touch', which dereferences it [weavec::null-dereference]
    touch(p);
  free(p);
}

// Clean: the edge is dead in the mathematics too.
void dead(void) {
  size_t i = 0;
  char *p = malloc(4);
  if (i != 0)
    touch(p);
  free(p);
}

// CHECK: 1 warning and 7 errors generated.
