// RFC 0013: this body is seen at compile time in a different translation unit.
#include "../../Inputs/prelude.h"
#include "heap13.h"
struct heap13_box *heap13_new(void) {
  struct heap13_box *b = malloc(sizeof *b); if (!b) return NULL;
  b->data = malloc(4);
  if (!b->data) { free(b); return NULL; }
  return b;
}
struct heap13_box *heap13_wrap(char *p) {
  struct heap13_box *b = malloc(sizeof *b); if (!b) return NULL;
  b->data = p; return b;
}

struct heap13_box *heap13_singleton;
void heap13_ensure(void) {
  if (heap13_singleton) return;
  heap13_singleton = malloc(sizeof *heap13_singleton);
  if (heap13_singleton) heap13_singleton->data = malloc(4);
}

void heap13_drop_child(void) { free(heap13_singleton->data); }

void heap13_swap(struct heap13_box *a, struct heap13_box *b) {
  char *old = a->data; a->data = b->data; b->data = old;
}
void heap13_reset(struct heap13_box *b) {
  struct heap13_box next = {malloc(4)};
  heap13_swap(b, &next); free(next.data);
}
void heap13_local_swap(struct heap13_box *a, struct heap13_box *b) {
  struct heap13_box *x = a, *y = b;
  char *old = x->data; x->data = y->data; y->data = old;
}
static void heap13_put(struct heap13_box *b, char *p) { b->data = p; }
void heap13_copy_and_free(struct heap13_box *a, struct heap13_box *b) {
  heap13_put(b, a->data); free(a->data);
}
