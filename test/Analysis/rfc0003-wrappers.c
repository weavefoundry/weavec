// RFC 0003: a function that frees (or moves) its parameter is summarised as
// consuming it, and callers are checked against that summary without any
// annotation, through arbitrarily deep wrappers and recursion.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

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
  // CHECK: rfc0003-wrappers.c:[[@LINE+1]]:10: error: use of 'n' after it was freed [weavec::use-after-free]
  return n->v;
  // CHECK: rfc0003-wrappers.c:[[@LINE-3]]:3: note: freed here
}

void double_free_through_wrapper(void) {
  struct node *n = node_new();
  node_free3(n);
  // CHECK: rfc0003-wrappers.c:[[@LINE+1]]:3: error: 'n' is freed twice [weavec::double-free]
  free(n);
  // CHECK: rfc0003-wrappers.c:[[@LINE-3]]:3: note: previously freed here
}

void recursion(struct node *a, struct node *b) {
  list_free(a);
  // CHECK: rfc0003-wrappers.c:[[@LINE+1]]:7: error: use of 'a' after it was freed [weavec::use-after-free]
  use(a);
  odd_free(b);
  // CHECK: rfc0003-wrappers.c:[[@LINE+1]]:7: error: use of 'b' after it was freed [weavec::use-after-free]
  use(b);
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
  struct node *n = make();
  drop(n);
  use(n);
}

void maybe(int c) {
  struct node *n = node_new();
  free_if(n, c);
  // CHECK: rfc0003-wrappers.c:[[@LINE+1]]:3: error: use of 'n' after it was freed [weavec::use-after-free]
  n->v = 1;
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

// CHECK: 5 errors generated.
