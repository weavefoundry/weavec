// RFC 0002: taking an address creates a loan held by the pointer variable;
// loans end when the holder is last used (RFC 0006) or reassigned. Freeing
// or moving a borrowed object is always a conflict; exclusivity between
// borrows (RFC 0001) is opt-in under `--exclusive-borrows` (RFC 0006,
// *Conflict rules*).
// RUN: not %weavec %s -- 2>&1 | FileCheck --check-prefixes=CHECK,LAX %s
// RUN: not %weavec --exclusive-borrows %s -- 2>&1 | FileCheck --check-prefixes=CHECK,EXCL %s
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
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:12: error: cannot borrow 'x' as mutable because it is already borrowed [weavec::conflicting-borrow]
  int *b = &x;
  // EXCL: rfc0002-borrows.c:[[@LINE-3]]:12: note: previous borrow of 'x' by 'a' here
  use(a);
  use(b);
}

void shared_then_mutable(void) {
  int x = 0;
  const int *a = &x;
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:12: error: cannot borrow 'x' as mutable because it is already borrowed [weavec::conflicting-borrow]
  int *b = &x;
  use((void *)a);
  use(b);
}

void mutable_then_shared(void) {
  int x = 0;
  int *a = &x;
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:18: error: cannot borrow 'x' as shared because it is already mutably borrowed [weavec::conflicting-borrow]
  const int *b = &x;
  use(a);
  use((void *)b);
}

void write_while_borrowed(void) {
  int x = 0;
  int *a = &x;
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot assign to 'x' while it is borrowed [weavec::conflicting-borrow]
  x = 1;
  // EXCL: rfc0002-borrows.c:[[@LINE-3]]:12: note: borrowed by 'a' here
  use(a);
}

void free_while_borrowed(void) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return;
  int *a = &n->v;
  free(n);
  // `&n->v` is a derived copy of `n` (RFC 0011): the use is the report.
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:7: error: use of 'a' after it was freed [weavec::use-after-free]
  use(a);
}

void move_while_borrowed(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  take(n);
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:7: error: use of 'a' after it was moved [weavec::use-after-move]
  use(a);
}

void temporary_borrows(void) {
  int x = 0;
  peek(&x);
  poke(&x); // fine: the borrow for peek ended with the call
  int *a = &x;
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot borrow 'x' as mutable because it is already borrowed [weavec::conflicting-borrow]
  poke(&x);
  use(a);
}

void array_decay(void) {
  int a[4];
  int *p = a;
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:12: error: cannot borrow 'a[*]' as mutable because it is already borrowed [weavec::conflicting-borrow]
  int *q = &a[1];
  use(p);
  use(q);
}

// A loan whose holder is still live when the object is freed conflicts in
// both modes (RFC 0006, *Loans end at the last use of their holder*).
void free_before_last_use(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  free(n);
  // CHECK: rfc0002-borrows.c:[[@LINE+1]]:4: error: use of 'a' after it was freed [weavec::use-after-free]
  *a = 1;
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

// Clean in both modes: the loan ends at the holder's last use, not at the
// end of its scope (RFC 0006).
void holder_dead(void) {
  int x = 0;
  int *a = &x;
  use(a);
  int *b = &x; // `a` is dead: no conflict even when exclusive
  use(b);
  x = 1; // `b` is dead too
}

void free_after_last_use(struct node *WEAVEC_OWNED n) {
  int *a = &n->v;
  *a = 1;
  free(n);
}

// Clean by default: a view of a buffer that another routine writes is the
// ordinary shape of C string handling (RFC 0006, *Bugs deliberately not
// caught*). Exclusivity rejects it.
void view_then_write(void) {
  int buf[8];
  int *p = buf;
  // EXCL: rfc0002-borrows.c:[[@LINE+1]]:3: error: cannot borrow 'buf[*]' as mutable because it is already borrowed [weavec::conflicting-borrow]
  poke(buf);
  use(p);
}

// LAX: 3 errors generated.
// EXCL: 10 errors generated.
