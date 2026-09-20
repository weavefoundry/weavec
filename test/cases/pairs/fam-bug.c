// RFC 0017: added regression pair. Only two non-byte tail elements were allocated; the sibling count cannot widen them.
// FLAGS: -std=c11
#include "Inputs/prelude.h"
struct block { size_t claimed; double alignment; int data[]; };
void run(void) {
  struct block *p = malloc(__builtin_offsetof(struct block, data) +
                           2 * sizeof(int));
  if (!p) return;
  p->claimed = 10;
  int *tail = p->data;
  tail[2] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
