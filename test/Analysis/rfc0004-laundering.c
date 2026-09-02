// RFC 0004, "Laundering": a raw pointer is brought into the model by
// asserting its ownership inside an unsafe region, either by storing it into
// a place declared with a safe kind or by returning it from a function whose
// return type is annotated. The same assertion outside an unsafe region is
// an unsafe-operation. A pointer leaves the model by conversion to an integer
// or by being stored into a WEAVEC_RAW place.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

typedef unsigned long uintptr_t;
struct node {
  int v;
};

// Assertion by return.
WEAVEC_OWNED struct node *by_return_outside(uintptr_t x) {
  // CHECK: rfc0004-laundering.c:[[@LINE+1]]:10: error: raw pointer is returned from a function whose return type is annotated WEAVEC_OWNED outside an unsafe region [weavec::unsafe-operation]
  return (struct node *)x;
}
WEAVEC_OWNED struct node *by_return_inside(uintptr_t x) {
  WEAVEC_UNSAFE { return (struct node *)x; }
}
void uses_by_return(uintptr_t x) {
  struct node *n = by_return_inside(x);
  n->v = 1; /* owned, per the callee's annotation */
  free(n);
  // CHECK: rfc0004-laundering.c:[[@LINE+1]]:7: error: use of 'n' after it was freed [weavec::use-after-free]
  use(n);
}

// Assertion by assignment to an annotated local.
void by_local_outside(uintptr_t x) {
  struct node *raw = (struct node *)x;
  // CHECK: rfc0004-laundering.c:[[@LINE+1]]:33: error: raw pointer 'raw' is assigned to 'n', which is declared WEAVEC_OWNED, outside an unsafe region [weavec::unsafe-operation]
  WEAVEC_OWNED struct node *n = raw;
  // CHECK: rfc0004-laundering.c:[[@LINE-3]]:22: note: 'raw' is raw: cast from an integer here
  n->v = 1; /* asserted anyway, so this does not cascade */
  free(n);
}
void by_local_inside(uintptr_t x) {
  WEAVEC_OWNED struct node *n;
  WEAVEC_UNSAFE { n = (struct node *)x; }
  n->v = 1;
  free(n);
  // CHECK: rfc0004-laundering.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  free(n);
}

// Assertion by assignment to an annotated field.
struct box {
  struct node *WEAVEC_OWNED owned;
};
void by_field(struct box *b, uintptr_t x) {
  // CHECK: rfc0004-laundering.c:[[@LINE+1]]:3: error: raw pointer is assigned to 'b->owned', which is declared WEAVEC_OWNED, outside an unsafe region [weavec::unsafe-operation]
  b->owned = (struct node *)x;
  WEAVEC_UNSAFE { b->owned = (struct node *)x; }
  free(b->owned);
  // CHECK: rfc0004-laundering.c:[[@LINE+1]]:7: error: use of 'b->owned' after it was freed [weavec::use-after-free]
  use(b->owned);
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

// CHECK: 6 errors generated.
