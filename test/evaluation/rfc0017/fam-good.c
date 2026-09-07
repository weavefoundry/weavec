// RFC 0017: added regression pair. Only two non-byte tail elements were allocated; the sibling count cannot widen them.
#include "../../Inputs/prelude.h"
struct block { size_t claimed; double alignment; int data[]; };
void run(void) {
  struct block *p = malloc(__builtin_offsetof(struct block, data) +
                           2 * sizeof(int));
  if (!p) return;
  p->claimed = 10;
  int *tail = p->data;
  tail[1] = 0;
  free(p);
}
