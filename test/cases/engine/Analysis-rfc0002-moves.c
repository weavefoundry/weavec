// Engine pin converted from test/Analysis/rfc0002-moves.c; markers are the v0.10.0 golden diagnostics.
// RFC 0002: passing an owned pointer to a WEAVEC_OWNED parameter moves it.
#include "Inputs/prelude.h"
#include <weavec.h>

void take(void *WEAVEC_OWNED p);

void use_after_move(int *WEAVEC_OWNED p) {
  take(p);
  use(p); // BUG: use-after-move
}

void free_after_move(void) {
  int *p = malloc(4);
  take(p);
  free(p); // BUG: use-after-move
}

void move_then_reinitialise(void) {
  int *p = malloc(4);
  take(p);
  p = malloc(4);
  free(p);
}
