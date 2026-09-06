// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
#include "Inputs/heap.c"
void run(void) {
  struct box *b = box_new();
  if (!b) return;
  b->data[3] = 0;
  free(b->data);
  free(b);
}
