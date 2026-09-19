// Engine pin converted from test/Analysis/rfc0006-nll.c; markers are the v0.10.0 golden diagnostics.
// RFC 0006, *Loans end at the last use of their holder*: a loan lives while
// its holder is live, not while it is in scope. Loans held through a
// pointer, by a global, or by an address-taken local last until the holder
// is reassigned.
#include "Inputs/prelude.h"
#include <weavec.h>

struct node {
  int v;
};

struct holder {
  int *view;
};

// Clean: the holder is dead when the object goes away.
void last_use(void) {
  char buf[8];
  char *p = buf;
  use(p);
  buf[0] = 0;
}

void free_after_last_use(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  *a = 1;
  free(n);
}

void loop_then_free(struct node *WEAVEC_OWNED n, int k) {
  int *a = &n->v;
  for (int i = 0; i < k; i++)
    *a += i;
  free(n);
}

// Reported: the holder is used after the object is freed. `&n->v` is a
// derived copy of `n` (RFC 0011), so the use through it is the report.
void still_live(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  free(n);
  *a = 1; // BUG: use-after-free definite
}

void live_around_loop(struct node *WEAVEC_OWNED n, int k) {
  int *a = &n->v;
  for (int i = 0; i < k; i++) {
    if (i == 5) {
      free(n);
      break;
    }
  }
  *a = 1; // BUG: use-after-free definite
}

// A holder that is not a plain local never expires on liveness, and the
// loan a derived copy carries (RFC 0011, *Derived pointers*) is what
// reports the free: nothing in this function reads the copy again.
void through_pointer(struct node *WEAVEC_OWNED n, int **out) {
  *out = &n->v;
  free(n); // BUG: conflicting-borrow definite
}

void through_field(struct node *WEAVEC_OWNED n, struct holder *h) {
  h->view = &n->v;
  free(n); // BUG: conflicting-borrow definite
}

void address_taken(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  int **pa = &a;
  free(n); // BUG: conflicting-borrow definite
  use(pa);
}

// The holder is live up to the statement that reads it: returning it or
// copying it out of a dying frame is still caught.
int *escape(void) {
  int x = 1;
  int *p = &x;
  return p; // BUG: lifetime-too-short definite
}
