// RFC 0041 §3, §5, rule escape: a parameter is borrowed for the call
// unless WEAVEC_ESCAPES; it is returned only through a lifetimebound
// parameter. The address of a local is never an error (§6).
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stddef.h>

struct node { struct node *next; int v; };
struct list { struct node *head; };
static struct node *last;

WEAVEC_SAFE void store(struct list *l, struct node *n) {
  l->head = n;                    // expected-error {{'n' is borrowed for this call and cannot be stored where it outlives the call [weavec-safe:escape]}}
  last = n;                       // expected-error {{[weavec-safe:escape]}}
  struct node *local = n;         // a local slot keeps it in the frame
  local->v = 1;
}

WEAVEC_SAFE void push(struct list *l, struct node *WEAVEC_ESCAPES n) {
  n->next = l->head;
  l->head = n;
}

WEAVEC_SAFE struct node *ret(struct node *n) {
  return n;                       // expected-error {{'n' is borrowed for this call and cannot be returned [weavec-safe:escape]}}
}

WEAVEC_SAFE struct node *pass(struct node *n WEAVEC_LIFETIMEBOUND) {
  return n->next ? n : n;
}

WEAVEC_SAFE struct node *through(struct node *m) {
  struct node *p = pass(m);
  return p;                       // expected-error {{'m' is borrowed}}
}

WEAVEC_SAFE void forward(struct list *l, struct node *n) {
  push(l, n);                     // expected-error {{'n' is borrowed for this call and cannot be passed to the escaping parameter 'n'}}
}

WEAVEC_SAFE void aggregate(struct node *n) {
  struct node *pair[2] = {n, n};  // expected-error 2 {{[weavec-safe:escape]}}
  (void)pair;
}

WEAVEC_SAFE int local(struct list *l) {
  struct node mine = {0, 1};
  push(l, &mine);                 // placed on the heap (§6), not an error
  return l->head->v;
}

WEAVEC_SAFE struct node *loaded(struct node *n) {
  return n->next;                 // loaded from memory: not borrowed
}
