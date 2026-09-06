// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
void make(char **p, size_t *n) { *n = 4; *p = malloc(*n); }
void run(void) {
  char *p; size_t n; make(&p, &n); if (!p) return;
  p[4] = 0; // BUG: output-size
  free(p);
}
