// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(void) {
  struct box *b = malloc(sizeof *b);
  if (!b) return;
  b->data = malloc(4);
  if (!b->data) { free(b); return; }
  b->data[4] = 0; // BUG: overflow
  free(b->data);
  free(b);
}
