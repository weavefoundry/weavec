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
  strcpy(dest, p->data); // BUG: out-of-bounds // TRAP: len
  free(p->data); free(p);
}
// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
int main(void) { run(); return 0; }
