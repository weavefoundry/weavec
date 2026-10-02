// RFC 0006: outcome-conditional summaries inferred in one unit are applied
// in another, through the whole-program database (RFC 0005).
//
// RUN: not %weavec --whole-program %s %S/Inputs/grow.c -- 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/grow.c -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

char *grow(char *p, size_t n);
int try_take(char *p, int c);

// The null class keeps `p`: RFC 0030 §8.2 releases it when the size is
// zero, which the zero-initialisation wrapper never asks for (RFC 0031
// *Implementation amendments*).
// DUMP: program:
// DUMP-NEXT: function 'grow':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result null when null
// DUMP-NEXT: result fresh#0 free extent param1 {{.*}}when nonnull
// DUMP-NEXT: move *param0 free when result nonnull
// DUMP: function 'try_take':
// DUMP-NEXT: always-returns
// DUMP-NEXT: result int [-1, -1] when negative and param 1 =0
// DUMP-NEXT: result int [0, 0] when zero and param 1 !=0
// DUMP-NEXT: release *param0 free when result zero

// Clean: the tests select the classes that did not consume. `grow(p, 16)`
// cannot release `p` on its null result: the numeric context the call asks
// of `grow.c` binds the size (RFC 0031 §7 *Amendment (cross-unit
// contexts)*).
void grown(char *p) {
  char *q = grow(p, 16);
  if (q == NULL) {
    free(p);
    return;
  }
  free(q);
}

void guarded(char *p, int c) {
  if (try_take(p, c) != 0)
    free(p);
}

// Reported: the wrong side, and no test at all. `try_take` releases `p`
// exactly when it returns zero, and `rc == 0` selects that class: the double
// free is definite (RFC 0031 §6.3, a pending case resolved by the test).
void wrong_side(char *p, int c) {
  int rc = try_take(p, c);
  if (rc == 0)
    // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:5: error: 'p' is freed twice [weavec::double-free]
    free(p);
}

void untested(char *p) {
  char *q = grow(p, 8);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:7: warning: use of 'p' after it may have been moved [weavec::use-after-move]
  use(p);
  free(q);
}

// CHECK: 1 warning and 1 error generated.
