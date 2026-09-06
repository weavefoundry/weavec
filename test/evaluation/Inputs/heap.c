// RFC 0013: constructor bodies shared by helper and separate-unit cases.
#include "heap.h"
struct box *box_new(void) {
  struct box *b = malloc(sizeof *b);
  if (!b) return NULL;
  b->data = malloc(4);
  if (!b->data) { free(b); return NULL; }
  return b;
}
struct box *box_wrap(char *p) {
  struct box *b = malloc(sizeof *b);
  if (!b) return NULL;
  b->data = p;
  return b;
}
