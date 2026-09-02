// RFC 0003: a definition's body is checked against its own annotations.
// Callers keep trusting the annotation; the definition is where the error is.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"
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
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:3: error: 'n' is annotated WEAVEC_BORROWED but is freed here [weavec::annotation-mismatch]
  free(n);
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE-3]]:50: note: 'n' is annotated here
}

void frees_alias(struct node *WEAVEC_BORROWED n) {
  struct node *m = n;
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:3: error: 'n' is annotated WEAVEC_BORROWED but is freed here [weavec::annotation-mismatch]
  free(m);
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE-4]]:47: note: 'n' is annotated here
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE-2]]:3: note: 'm' is a copy of 'n'
}

void moves_mut(struct node *WEAVEC_MUT n) {
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:3: error: 'n' is annotated WEAVEC_MUT but is moved here [weavec::annotation-mismatch]
  take(n);
}

void writes_borrowed(struct node *WEAVEC_BORROWED n) {
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:3: error: 'n' is annotated WEAVEC_BORROWED but is written through here [weavec::annotation-mismatch]
  n->v = 1;
}

void frees_field_of_borrowed(struct buf *WEAVEC_BORROWED b) {
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:3: error: 'b' is annotated WEAVEC_BORROWED but 'b->data' is freed here [weavec::annotation-mismatch]
  free(b->data);
}

void lends_borrowed_as_mut(struct node *WEAVEC_BORROWED n) {
  // CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:3: error: 'n' is annotated WEAVEC_BORROWED but is written through here [weavec::annotation-mismatch]
  poke(n);
}

// CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:59: error: function returns a borrow but its return type is annotated WEAVEC_OWNED [weavec::annotation-mismatch]
char *WEAVEC_OWNED returns_borrow(struct buf *b) { return (char *)&b->data; }
// CHECK: rfc0003-annotation-mismatch.c:[[@LINE-1]]:20: note: annotated here

// CHECK: rfc0003-annotation-mismatch.c:[[@LINE+1]]:52: error: function returns a fresh allocation but its return type is annotated WEAVEC_BORROWED [weavec::annotation-mismatch]
char *WEAVEC_BORROWED returns_fresh(void) { return malloc(4); }

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

// CHECK: 8 errors generated.
