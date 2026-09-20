// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
#include "Inputs/heap.c"
void run(void) {
  struct box *b = box_new();
  if (!b) return;
  free(b); // BUG: leak
}
