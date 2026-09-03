// RFC 0006, *Loans end at the last use of their holder*: a loan lives while
// its holder is live, not while it is in scope. Loans held through a
// pointer, by a global, or by an address-taken local last until the holder
// is reassigned.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
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

// Reported: the holder is used after the object is freed.
void still_live(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  // CHECK: rfc0006-nll.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  free(n);
  // CHECK: rfc0006-nll.c:[[@LINE-3]]:12: note: borrowed by 'a' here
  *a = 1;
}

void live_around_loop(struct node *WEAVEC_OWNED n, int k) {
  int *a = &n->v;
  for (int i = 0; i < k; i++) {
    if (i == 5) {
      // CHECK: rfc0006-nll.c:[[@LINE+1]]:7: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
      free(n);
      break;
    }
  }
  *a = 1;
}

// A holder that is not a plain local never expires on liveness.
void through_pointer(struct node *WEAVEC_OWNED n, int **out) {
  *out = &n->v;
  // CHECK: rfc0006-nll.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  free(n);
}

void through_field(struct node *WEAVEC_OWNED n, struct holder *h) {
  h->view = &n->v;
  // CHECK: rfc0006-nll.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  free(n);
}

void address_taken(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  int **pa = &a;
  // CHECK: rfc0006-nll.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  free(n);
  use(pa);
}

// The holder is live up to the statement that reads it: returning it or
// copying it out of a dying frame is still caught.
int *escape(void) {
  int x = 1;
  int *p = &x;
  // CHECK: rfc0006-nll.c:[[@LINE+1]]:10: error: 'p' may outlive 'x', which it points to [weavec::lifetime-too-short]
  return p;
}

// CHECK: 6 errors generated.
