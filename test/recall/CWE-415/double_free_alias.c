// CWE-415: double free through an alias and through a callee.
#include "recall.h"

struct holder {
  char *buf;
};

static void release(struct holder *h) { free(h->buf); }

void bad_alias(void) {
  char *data = malloc(100);
  if (!data)
    return;
  char *alias = data;
  free(alias);
  free(data); // RECALL: double-free
}

void bad_callee(void) {
  struct holder h;
  h.buf = malloc(100);
  if (!h.buf)
    return;
  release(&h);
  free(h.buf); // RECALL: double-free
}

void bad_container(void) {
  struct wrapped { int tag; char payload[8]; } *w = malloc(sizeof *w);
  if (!w)
    return;
  free(w);
  free(w); // RECALL: double-free
}
