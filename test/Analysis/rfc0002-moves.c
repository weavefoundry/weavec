// RFC 0002: passing an owned pointer to a WEAVEC_OWNED parameter moves it.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

void take(void *WEAVEC_OWNED p);

void use_after_move(int *WEAVEC_OWNED p) {
  take(p);
  // CHECK: rfc0002-moves.c:[[@LINE+1]]:7: error: use of 'p' after it was moved [weavec::use-after-move]
  use(p);
  // CHECK: rfc0002-moves.c:[[@LINE-3]]:3: note: moved here
}

void free_after_move(void) {
  int *p = malloc(4);
  take(p);
  // CHECK: rfc0002-moves.c:[[@LINE+1]]:3: error: use of 'p' after it was moved [weavec::use-after-move]
  free(p);
}

void move_then_reinitialise(void) {
  int *p = malloc(4);
  take(p);
  p = malloc(4);
  free(p);
}

// CHECK: 2 errors generated.
