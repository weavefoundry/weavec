// RFC 0013: fixed evaluation case.
#include "Inputs/heap.h"
#include "Inputs/heap.c"
char *strcpy(char *, const char *);
struct box *make(void) {
  struct box *p = box_new(); if (!p) return NULL;
  strcpy(p->data, "abc"); return p;
}
void run(void) {
  struct box *p = make(); if (!p) return;
  char dest[3];
  strcpy(dest, p->data); // BUG: string-field
  free(p->data); free(p);
}
