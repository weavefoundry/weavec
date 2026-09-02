// RFC 0003: summaries record where pointer values flow (stores into
// caller-visible memory, returned copies and borrows), so callers see escapes
// and aliases created inside callees.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

struct node {
  int v;
  struct node *next;
};

static char *g;
static void keep(char *p) { g = p; }
static void store_in(char **slot, char *p) { *slot = p; }
static struct node *next_of(struct node *n) { return n->next; }
static int *field_of(struct node *n) { return &n->v; }
static char *through(char *p) { return p; }

void escape_local(void) {
  char local[8];
  // CHECK: rfc0003-escapes.c:[[@LINE+1]]:3: error: 'g' may outlive 'local', which it points to [weavec::lifetime-too-short]
  keep(local);
  // CHECK: rfc0003-escapes.c:[[@LINE-3]]:8: note: 'local' is declared here
}

void escape_through_out_param(char **slot) {
  int x;
  // CHECK: rfc0003-escapes.c:[[@LINE+1]]:3: error: '*slot' may outlive 'x', which it points to [weavec::lifetime-too-short]
  store_in(slot, (char *)&x);
}

void escape_fine(char *outer) {
  keep(outer);
  keep(malloc(8));
}

void result_copies_field(struct node *n) {
  struct node *m = next_of(n);
  free(n->next);
  // CHECK: rfc0003-escapes.c:[[@LINE+1]]:7: error: use of 'm' after it was freed [weavec::use-after-free]
  use(m);
}

void result_copies_argument(void) {
  char *p = malloc(8);
  char *q = through(p);
  free(q);
  // CHECK: rfc0003-escapes.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

void result_borrows_field(void) {
  int *v;
  {
    struct node n;
    // CHECK: rfc0003-escapes.c:[[@LINE+1]]:5: error: 'v' may outlive 'n', which it points to [weavec::lifetime-too-short]
    v = field_of(&n);
  }
  use(v);
}

// Landed with RFC 0003: a plain copy of a pointer holding a loan is checked
// like a fresh borrow (previously only `&x` was).
static int *global_int;
void copied_loan(void) {
  int x = 0;
  int *p = &x;
  // CHECK: rfc0003-escapes.c:[[@LINE+1]]:3: error: 'global_int' may outlive 'x', which it points to [weavec::lifetime-too-short]
  global_int = p;
}

// CHECK: 6 errors generated.
