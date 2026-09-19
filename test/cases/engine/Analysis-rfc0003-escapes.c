// Engine pin converted from test/Analysis/rfc0003-escapes.c; markers are the v0.10.0 golden diagnostics.
// RFC 0003: summaries record where pointer values flow (stores into
// caller-visible memory, returned copies and borrows), so callers see escapes
// and aliases created inside callees.
#include "Inputs/prelude.h"

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
  keep(local); // BUG: lifetime-too-short definite
}

void escape_through_out_param(char **slot) {
  int x;
  store_in(slot, (char *)&x); // BUG: lifetime-too-short definite
}

void escape_fine(char *outer) {
  keep(outer);
  keep(malloc(8));
}

void result_copies_field(struct node *n) {
  struct node *m = next_of(n);
  free(n->next);
  use(m); // BUG: use-after-free definite
}

void result_copies_argument(void) {
  char *p = malloc(8);
  char *q = through(p);
  free(q);
  use(p); // BUG: use-after-free definite
}

void result_borrows_field(void) {
  int *v;
  {
    struct node n;
    v = field_of(&n); // BUG: lifetime-too-short definite
  }
  use(v);
}

// Landed with RFC 0003: a plain copy of a pointer holding a loan is checked
// like a fresh borrow (previously only `&x` was).
static int *global_int;
void copied_loan(void) {
  int x = 0;
  int *p = &x;
  global_int = p; // BUG: lifetime-too-short definite
}
