// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void run(void) {
  struct box *b = box_new();
  if (!b) return;
  b->data[4] = 0; // BUG: overflow
  free(b->data);
  free(b);
}
