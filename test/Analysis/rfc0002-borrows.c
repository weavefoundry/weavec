// RFC 0002: taking an address creates a loan held by the pointer variable;
// loans conflict per RFC 0001 and end when the holder dies or is reassigned.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
#include <weavec.h>

void peek(const int *WEAVEC_BORROWED p);
void poke(int *WEAVEC_MUT p);
void take(void *WEAVEC_OWNED p);

struct node {
  int v;
};

void two_mutable(void) {
  int x = 0;
  int *a = &x;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:12: error: cannot borrow 'x' as mutable because it is already borrowed [weavec::conflicting-borrow]
  int *b = &x;
  // CHECK: rfc0002-borrows.c:[[@LINE-3]]:12: note: previous borrow of 'x' by 'a' here
  use(a);
  use(b);
}

void shared_then_mutable(void) {
  int x = 0;
  const int *a = &x;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:12: error: cannot borrow 'x' as mutable because it is already borrowed [weavec::conflicting-borrow]
  int *b = &x;
  use((void *)a);
  use(b);
}

void mutable_then_shared(void) {
  int x = 0;
  int *a = &x;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:18: error: cannot borrow 'x' as shared because it is already mutably borrowed [weavec::conflicting-borrow]
  const int *b = &x;
  use(a);
  use((void *)b);
}

void write_while_borrowed(void) {
  int x = 0;
  int *a = &x;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot assign to 'x' while it is borrowed [weavec::conflicting-borrow]
  x = 1;
  // CHECK: rfc0002-borrows.c:[[@LINE-3]]:12: note: borrowed by 'a' here
  use(a);
}

void free_while_borrowed(void) {
  struct node *n = malloc(sizeof *n);
  int *a = &n->v;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot free 'n' while it is borrowed [weavec::conflicting-borrow]
  free(n);
  use(a);
}

void move_while_borrowed(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot move 'n' while it is borrowed [weavec::conflicting-borrow]
  take(n);
  use(a);
}

void temporary_borrows(void) {
  int x = 0;
  peek(&x);
  poke(&x); // fine: the borrow for peek ended with the call
  int *a = &x;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot borrow 'x' as mutable because it is already borrowed [weavec::conflicting-borrow]
  poke(&x);
  use(a);
}

void array_decay(void) {
  int a[4];
  int *p = a;
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:12: error: cannot borrow 'a[*]' as mutable because it is already borrowed [weavec::conflicting-borrow]
  int *q = &a[1];
  use(p);
  use(q);
}

// Clean: shared borrows coexist, writes through the borrow are fine, and
// loans end with their holder.
void shared_borrows(void) {
  int x = 0;
  const int *a = &x;
  const int *b = &x;
  use((void *)a);
  use((void *)b);
}

void write_through(void) {
  int x = 0;
  int *a = &x;
  *a = 5;
  use(a);
}

void holder_reassigned(void) {
  int x = 0;
  int *a = &x;
  a = NULL;
  x = 1;
  use(a);
}

void holder_out_of_scope(void) {
  int x = 0;
  {
    int *a = &x;
    use(a);
  }
  x = 1;
}

void holder_per_iteration(int n) {
  int x = 0;
  for (int i = 0; i < n; ++i) {
    int *a = &x;
    use(a);
  }
}

// CHECK: 8 errors generated.
