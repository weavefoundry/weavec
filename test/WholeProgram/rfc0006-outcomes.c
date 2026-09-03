// RFC 0006: outcome-conditional summaries inferred in one unit are applied
// in another, through the whole-program database (RFC 0005).
//
// RUN: not %weavec --whole-program %s %S/Inputs/grow.c -- 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/grow.c -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"

char *grow(char *p, size_t n);
int try_take(char *p, int c);

// DUMP: function 'grow': param 0: moved; stores{} returns{fresh, null} outcome null{} outcome nonnull{param 0: moved}
// DUMP: function 'try_take': param 0: freed; stores{} returns{} outcome zero{param 0: freed} outcome negative{}

// Clean: the tests select the classes that did not consume.
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

// Reported: the wrong side, and no test at all.
void wrong_side(char *p, int c) {
  int rc = try_take(p, c);
  if (rc == 0)
    // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:5: error: 'p' is freed twice [weavec::double-free]
    free(p);
}

void untested(char *p) {
  char *q = grow(p, 8);
  // CHECK: rfc0006-outcomes.c:[[@LINE+1]]:7: error: use of 'p' after it was moved [weavec::use-after-move]
  use(p);
  free(q);
}

// CHECK: 2 errors generated.
