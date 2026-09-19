// Engine pin converted from test/Annotations/rfc0003-annotation-mismatch.c; markers are the v0.10.0 golden diagnostics.
// RFC 0003: a definition's body is checked against its own annotations.
// Callers keep trusting the annotation; the definition is where the error is.
#include "Inputs/prelude.h"
#include <weavec.h>

struct node {
  int v;
  struct node *next;
};
struct buf {
  char *data;
};

void take(struct node *WEAVEC_OWNED n);
void poke(struct node *WEAVEC_MUT n);

void frees_borrowed(struct node *WEAVEC_BORROWED n) {
  free(n); // BUG: annotation-mismatch definite
}

void frees_alias(struct node *WEAVEC_BORROWED n) {
  struct node *m = n;
  free(m); // BUG: annotation-mismatch definite
}

void moves_mut(struct node *WEAVEC_MUT n) {
  take(n); // BUG: annotation-mismatch definite
}

void writes_borrowed(struct node *WEAVEC_BORROWED n) {
  n->v = 1; // BUG: annotation-mismatch definite
}

void frees_field_of_borrowed(struct buf *WEAVEC_BORROWED b) {
  free(b->data); // BUG: annotation-mismatch definite
}

void lends_borrowed_as_mut(struct node *WEAVEC_BORROWED n) {
  poke(n); // BUG: annotation-mismatch definite
}

char *WEAVEC_OWNED returns_borrow(struct buf *b) { return (char *)&b->data; } // BUG: annotation-mismatch definite

char *WEAVEC_BORROWED returns_fresh(void) { return malloc(4); } // BUG: annotation-mismatch definite

void consistent(struct node *WEAVEC_MUT n, const struct buf *WEAVEC_BORROWED b,
                struct node *WEAVEC_OWNED o) {
  n->v = 1;
  use(b->data);
  take(o);
}

char *WEAVEC_OWNED returns_fresh_ok(void) { return malloc(4); }
char *WEAVEC_BORROWED returns_field_ok(struct buf *b) { return b->data; }

// The caller believes the annotation; the lie is reported once, above.
void caller(struct node *n) {
  frees_borrowed(n);
  use(n);
}
