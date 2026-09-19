// RFC 0013: fixed evaluation case.
// CLEAN
// UNITS: Inputs/heap.c
#include "Inputs/heap.h"
void run(void) {
  struct box *b = box_new();
  if (!b) return;
  b->data[3] = 0;
  free(b->data);
  free(b);
}
