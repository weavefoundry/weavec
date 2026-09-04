// RFC 0009, *Inferred `noreturn`*: `never-returns` inferred in one unit ends
// paths in another, through the whole-program database (RFC 0005).
//
// RUN: not %weavec --whole-program %s %S/Inputs/die.c -- 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/die.c -- 2>&1 | FileCheck --check-prefix=DUMP %s
//
// Alone, the calls are boundaries and `fail` is assumed to return.
// RUN: not %weavec %s -- 2>&1 | FileCheck --check-prefix=ALONE %s
#include "../Inputs/prelude.h"

void die(const char *msg);
void fail(int code);
void check(int ok);

// DUMP-LABEL: function 'die':
// DUMP: summary: never-returns; *msg: read; stores{} returns{}
// DUMP-LABEL: function 'fail':
// DUMP: summary: never-returns; stores{} returns{}
// DUMP-LABEL: function 'check':
// DUMP: summary: stores{} returns{}

// Clean with the program: the bad path never reaches the use.
void good(int bad) {
  char *q = malloc(8);
  if (bad) {
    free(q);
    fail(bad);
  }
  // ALONE: rfc0009-noreturn-units.c:[[@LINE+1]]:7: error: use of 'q' after it was freed [weavec::use-after-free]
  use(q);
  free(q);
}

// Reported either way: `check` may return.
void checked(int bad) {
  char *q = malloc(8);
  if (bad) {
    free(q);
    check(bad);
  }
  // CHECK: rfc0009-noreturn-units.c:[[@LINE+1]]:7: error: use of 'q' after it was freed [weavec::use-after-free]
  use(q);
  free(q);
}

// CHECK-NOT: good
