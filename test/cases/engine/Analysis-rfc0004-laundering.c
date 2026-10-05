// Engine pin converted from test/Analysis/rfc0004-laundering.c; markers are the v0.10.0 golden diagnostics.
// RFC 0004, "Laundering": a raw pointer is brought into the model by
// asserting its ownership inside an unsafe region, either by storing it into
// a place declared with a safe kind or by returning it from a function whose
// return type is annotated. The same assertion outside an unsafe region is
// an unsafe-operation. A pointer leaves the model by conversion to an integer
// or by being stored into a WEAVEC_RAW place. RFC 0033 §2: a pointer converted
// from an integer is no longer raw, so the raw sources here are declared.
#include "Inputs/prelude.h"
#include <weavec.h>

typedef unsigned long uintptr_t;
struct node {
  int v;
};

// Assertion by return.
WEAVEC_OWNED struct node *by_return_outside(struct node *WEAVEC_RAW x) {
  return x; // BUG: unsafe-operation
}
WEAVEC_OWNED struct node *by_return_inside(uintptr_t x) {
  WEAVEC_UNSAFE { return (struct node *)x; }
}
void uses_by_return(uintptr_t x) {
  struct node *n = by_return_inside(x);
  n->v = 1; /* owned, per the callee's annotation */
  free(n);
  use(n); // BUG: use-after-free
}

// Assertion by assignment to an annotated local.
void by_local_outside(struct node *WEAVEC_RAW x) {
  struct node *raw = x;
  WEAVEC_OWNED struct node *n = raw; // BUG: unsafe-operation
  n->v = 1; /* asserted anyway, so this does not cascade */
  free(n);
}
void by_local_inside(struct node *WEAVEC_RAW x) {
  WEAVEC_OWNED struct node *n;
  WEAVEC_UNSAFE { n = x; }
  n->v = 1;
  free(n);
  free(n); // BUG: double-free
}

// Assertion by assignment to an annotated field.
struct box {
  struct node *WEAVEC_OWNED owned;
};
void by_field(struct box *b, struct node *WEAVEC_RAW x) {
  b->owned = x; // BUG: unsafe-operation
  WEAVEC_UNSAFE { b->owned = x; }
  free(b->owned);
  use(b->owned); // BUG: use-after-free
}

// Leaving the model.
uintptr_t out_by_integer(struct node *WEAVEC_OWNED n) {
  return (uintptr_t)n; /* silent: the value is gone; nothing to check */
}
struct ctx {
  void *WEAVEC_RAW cookie;
};
void out_by_raw_place(struct ctx *c) {
  struct node *n = malloc(sizeof *n);
  c->cookie = n; /* a copy into a raw place: n is still owned here */
  free(n);
}
void back_from_raw_place(struct ctx *c) {
  WEAVEC_OWNED struct node *n;
  WEAVEC_UNSAFE { n = c->cookie; }
  free(n);
}
