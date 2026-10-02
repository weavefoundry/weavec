// RFC 0009, *Scalar facts in the state* and *Refuting guards in the state*:
// a test of an integer place refines what is known about it, a move or a
// held resource made under a fact carries it as a guard, and a later test
// that contradicts the guard drops the record.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
// RUN: %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

struct buf {
  char *data;
  int owned;
};

// Correct code: two tests of one integer are one test. The old engine
// carried the test as a guard on the move in its state and proved these
// uses. The object engine keys a release by the parameter's test only in the
// summary (RFC 0031 *Implementation amendments*, *Pending cases and exit
// splitting*, `paramGuard`, spelled `lossy ... when param <i> ...`); in the
// state the object is `may-released` after the join, so each use is a
// possible finding: RFC 0030's accepted correlated-conditions false positive
// (*Accepted false positives and false traps*, probe 41e), which RFC 0031
// *Accepted false positives* keeps.

// DUMP-LABEL: function 'truthy':
// DUMP: release *param1 free lossy may when param 0 !=0
void truthy(int c, char *p) {
  if (c)
    free(p);
  if (!c)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// DUMP-LABEL: function 'eqzero':
// DUMP: release *param1 free lossy may when param 0 =0
void eqzero(int n, char *p) {
  if (n == 0)
    free(p);
  if (n != 0)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// A parameter test is a zero test: `n > 0` and `n == 3` key the release by
// `n != 0`, lossily (RFC 0031 §6.1).
// DUMP-LABEL: function 'sign':
// DUMP: release *param1 free lossy may when param 0 !=0
void sign(int n, char *p) {
  if (n > 0)
    free(p);
  if (n <= 0)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// DUMP-LABEL: function 'constant':
// DUMP: release *param1 free lossy may when param 0 !=0
void constant(int n, char *p) {
  if (n == 3)
    free(p);
  if (n == 4)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// DUMP-LABEL: function 'switched':
// DUMP: release *param1 free lossy may when param 0 =0
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
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
    break;
  default:
    break;
  }
}

// A local assigned a constant: the `if (c)` edge is infeasible and the move
// never happens. The summary says nothing about `c` (it is not the caller's).
// DUMP-LABEL: function 'local_constant':
// DUMP: release *param0 free when always
void local_constant(char *p) {
  int c = 0;
  if (c)
    free(p);
  use(p);
  free(p);
}

// A field of the caller's object: no parameter test keys the release.
// DUMP-LABEL: function 'field':
// DUMP: release *param0->data free may when always
void field(struct buf *b) {
  if (b->owned)
    free(b->data);
  if (!b->owned)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'b->data' after it may have been freed [weavec::use-after-free]
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
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// Reported: the flag was reassigned from an unknown value, which drops the
// guard's conjunct and leaves the move unconditional.
void reassigned(int c, int d, char *p) {
  if (c)
    free(p);
  c = d;
  if (!c)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// Reported: a computed expression establishes nothing about `n`.
void computed(int n, char *p) {
  if (n == 0)
    free(p);
  if ((n & 1) != 0)
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: warning: use of 'p' after it may have been freed [weavec::use-after-free]
    use(p);
}

// Reported: a guarded resource whose guard nothing refutes is still lost,
// at the branch that allocated it (RFC 0031 *Implementation amendments*,
// *Leaks on some paths*).
int leaked(int c) {
  char *p = NULL;
  if (c)
    // CHECK: rfc0009-scalars.c:[[@LINE+2]]:5: warning: 'p' is leaked [weavec::leak]
    // CHECK: rfc0009-scalars.c:[[@LINE+1]]:9: note: allocated here
    p = malloc(8);
  return 0;
}

// RFC 0017: comparisons use the operands' C types, including full-width
// unsigned constants. `ULONG_MAX` is not signed `-1`, so `i > ULONG_MAX`
// fails; `(size_t)-1` is exactly SIZE_MAX, so the sentinel test succeeds.
static void touch(char *p) { p[0] = 1; }

int above_max(void) {
  size_t i = 0;
  char *p = malloc(4);
  if (i > 18446744073709551615UL) { free(p); return 1; }
  touch(p);
  free(p);
  return 0;
}

int sentinel(void) {
  size_t n = (size_t)-1;
  char *p = malloc(4);
  if (n == (size_t)-1) { free(p); return 1; }
  // Clean: the exact sentinel comparison makes this dereference unreachable.
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
    touch(p);
  free(p);
  p = malloc(4);
  if (y > 5u)
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

// CHECK: 10 warnings generated.
