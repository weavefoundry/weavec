// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
struct pair { char *a, *b; };
struct pair *make(void) {
  struct pair *p = malloc(sizeof *p); if (!p) return NULL;
  p->a = malloc(4); if (!p->a) { free(p); return NULL; }
  p->b = p->a; return p;
}
void run(void) {
  struct pair *p = make(); if (!p) return;
  free(p->a);
  p->b[0] = 0; // BUG: shared-child
  free(p);
}
