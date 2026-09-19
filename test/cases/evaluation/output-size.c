// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void make(char **p, size_t *n) { *n = 4; *p = malloc(*n); }
void run(void) {
  char *p; size_t n; make(&p, &n); if (!p) return;
  p[4] = 0; // BUG: out-of-bounds // TRAP: index
  free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
