// Engine pin converted from test/Analysis/rfc0003-wrappers.c; markers are the v0.10.0 golden diagnostics.
// RFC 0003: a function that frees (or moves) its parameter is summarised as
// consuming it, and callers are checked against that summary without any
// annotation, through arbitrarily deep wrappers and recursion.
#include "Inputs/prelude.h"

struct node {
  int v;
  struct node *next;
};

static struct node *node_new(void) { return malloc(sizeof(struct node)); }
static void node_free(struct node *n) { free(n); }
static void node_free2(struct node *n) { node_free(n); }
static void node_free3(struct node *n) {
  if (n)
    node_free2(n);
}

static void list_free(struct node *n) {
  if (!n)
    return;
  list_free(n->next);
  free(n);
}

static void even_free(struct node *n);
static void odd_free(struct node *n) {
  if (n)
    even_free(n->next);
  free(n);
}
static void even_free(struct node *n) {
  if (n)
    odd_free(n->next);
  free(n);
}

int use_after_wrapper(void) {
  struct node *n = node_new();
  node_free(n);
  return n->v; // BUG: use-after-free
}

void double_free_through_wrapper(void) {
  struct node *n = node_new();
  node_free3(n);
  free(n); // BUG: double-free
}

void recursion(struct node *a, struct node *b) {
  list_free(a);
  use(a); // BUG: use-after-free
  odd_free(b);
  use(b); // BUG: use-after-free
}

// Unresolvable arguments are dropped, and conditional frees are may-frees.
static int free_if(struct node *n, int c) {
  if (c) {
    free(n);
    return 1;
  }
  return 0;
}

void fine(struct node *(*make)(void), void (*drop)(struct node *)) {
  node_free(NULL);
  node_free(node_new());
  // RFC 0030 §9.3: an indirect call through a slot with no known target is
  // the §5.1 default under the reason `callback`.
  struct node *n = make(); // UNRESOLVED: temporal:callback
  drop(n); // UNRESOLVED: temporal:callback
  use(n);
}

void maybe(int c) {
  struct node *n = node_new();
  free_if(n, c);
  n->v = 1; // BUG: use-after-free
}

// Testing the result that tells the paths apart retracts the may-free on
// the path that did not free (RFC 0006, *Outcome-conditional summaries*).
void tested(int c) {
  struct node *n = node_new();
  if (!n)
    return;
  if (free_if(n, c))
    return;
  n->v = 1;
  free(n);
}
