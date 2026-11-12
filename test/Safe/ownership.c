// RFC 0043 §3: ownership's rules, each with accepted and rejected forms:
// owner-moved, owner-leak, owner-source, owner-path, owner-slot,
// stale-borrow and call-alias.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item { char *WEAVEC_OWNED name; int v; };
struct list { struct item *WEAVEC_OWNED *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) items; size_t len, cap; };
struct node { struct node *WEAVEC_OWNED next; struct node *prev; int v; };
struct cache { struct item *last; };
struct item *WEAVEC_OWNED item_new(int v);
void item_free(struct item *WEAVEC_OWNED it);
void item_touch(struct item *it);
void list_grow(struct list *l);
void list_look(const struct list *l);
void list_put(struct list *l, struct item *it);
void node_drop(struct node *WEAVEC_OWNED n);
struct item *WEAVEC_OWNED global_item;

// owner-moved
int afterRelease(void) {
  struct item *it = item_new(1);
  if (it == NULL)
    return -1;
  item_free(it);
  return it->v; // expected-error {{'it' is used after it was moved to 'item_free' [weavec-safe:owner-moved]}}
}

void twice(void) {
  char *p = malloc(4);
  free(p);
  free(p); // expected-error {{'p' is used after it was released by 'free' [weavec-safe:owner-moved]}}
}

int maybeReleased(int c) {
  struct item *it = item_new(1);
  if (it == NULL)
    return -1;
  if (c)
    item_free(it);
  int v = it->v; // expected-error {{'it' is used after it may have been moved to 'item_free' [weavec-safe:owner-moved]}}
  if (!c)
    item_free(it);
  return v;
}

void leftMoved(struct item *it) {
  free(it->name);
} // expected-error {{'it->name' is left moved when 'leftMoved' returns [weavec-safe:owner-moved]}}

void leftWhole(struct item *it) {
  free(it->name);
  it->name = NULL;
}

int useBeforeTest(void) {
  char *p = malloc(4);
  if (p == NULL)
    return -1;
  char *d = realloc(p, 8);
  p[0] = 1; // expected-error {{'p' is used before 'd', the result of 'realloc', was tested: the call released 'p' if 'd' is not null [weavec-safe:owner-moved]}}
  if (d == NULL) {
    free(p);
    return -1;
  }
  free(d);
  return 0;
}

// owner-leak
int earlyReturn(int c) {
  struct item *it = item_new(4);
  if (c > 100)
    return -2; // expected-error {{returning while 'it' may still own its block [weavec-safe:owner-leak]}}
  item_free(it);
  return 0;
}

void member(struct item *WEAVEC_OWNED it) {
  free(it); // expected-error {{'it->name' may still own its block when 'it' is released [weavec-safe:owner-leak]}}
}

void memberReleased(struct item *WEAVEC_OWNED it) {
  if (it == NULL)
    return;
  free(it->name);
  free(it);
}

void overwrite(struct item *it, const char *WEAVEC_NULL_TERMINATED s) {
  it->name = strdup(s); // expected-error {{'it->name' is assigned over while it may still own its block [weavec-safe:owner-leak]}}
}

void setter(struct item *it, const char *WEAVEC_NULL_TERMINATED s) {
  char *copy = strdup(s);
  if (copy == NULL)
    return;
  free(it->name);
  it->name = copy;
}

void scope(int c) {
  if (c) {
    char *p = malloc(4);
    (void)p;
  } // expected-error {{'p' goes out of scope while it may still own its block [weavec-safe:owner-leak]}}
}

void discarded(void) {
  strdup("x"); // expected-error {{the result of 'strdup' owns a block and is not kept in an owner [weavec-safe:owner-leak]}}
}

void noOwner(struct cache *c) {
  c->last = item_new(1); // expected-error {{'item_new' returns an owner and 'c->last' is not one: the block would have no owner [weavec-safe:owner-leak]}}
}

int classic(char *WEAVEC_OWNED p) {
  p = realloc(p, 64); // expected-error {{'p' is assigned over while it may still own its block [weavec-safe:owner-leak]}}
  free(p);
  return 0;
}

char *notOwnedResult(void) {
  char *p = malloc(4);
  return p; // expected-error {{'p' owns a block and 'notOwnedResult' does not return an owner [weavec-safe:owner-leak]}}
}

int lines(FILE *f) {
  char *line = NULL;
  size_t cap = 0;
  int n = 0;
  while (getline(&line, &cap, f) > 0)
    n++;
  free(line);
  return n;
}

void exits(void) {
  char *p = malloc(4);
  if (p == NULL)
    exit(1);
  exit(0); // nothing is dropped by a call that does not return
}

// owner-source
void borrowedIn(struct item *it, char *s) {
  free(it->name);
  it->name = s; // expected-error {{'it->name' is an owner and 's' is borrowed for this call [weavec-safe:owner-source]}} expected-error {{[weavec-safe:escape]}}
}

void literal(struct item *it) {
  free(it->name);
  it->name = "x"; // expected-error {{'it->name' is an owner and a string literal is not a heap block [weavec-safe:owner-source]}}
}

void stepped(void) {
  char *p = malloc(4);
  p++; // expected-error {{'p' is an owner: arithmetic would leave it pointing inside its block [weavec-safe:owner-source]}}
  free(p);
}

void releaseBorrowed(char *p) {
  free(p); // expected-error {{'p' is released by 'free' and is not an owner [weavec-safe:owner-source]}}
}

void bytes(struct item *a, struct item *b) {
  memcpy(a, b, sizeof *a); // expected-error {{copying 'struct item' as bytes would leave two owners of its blocks [weavec-safe:owner-source]}}
}

// owner-path
void unlinkPrev(struct node *n) {
  struct node *WEAVEC_OWNED next = n->prev->next; // expected-error {{'n->prev->next' is reached through the reference 'n->prev' and cannot be moved or released from here [weavec-safe:owner-path]}}
  n->prev->next = NULL; // expected-error {{[weavec-safe:owner-path]}}
  node_drop(next);
}

void throughConst(const struct item *it) {
  free(it->name); // expected-error {{'it->name' is reached through a pointer to const ('it') and cannot be moved or released [weavec-safe:owner-path]}}
}

void inPlace(void) {
  free(global_item->name); // expected-error {{'global_item->name' is below the global 'global_item', read in place, and cannot be moved or released [weavec-safe:owner-path]}}
}

void movedOut(void) {
  struct item *WEAVEC_OWNED it = global_item;
  global_item = NULL;
  free(it->name);
  it->name = NULL;
  global_item = it;
}

void passReference(struct node *n) {
  list_grow((struct list *)0);
  (void)n;
}

// owner-slot
void slot(struct list *l, size_t i) {
  item_free(l->items[i]); // expected-error {{'l->items[i]' is moved out and still holds the pointer [weavec-safe:owner-slot]}}
}

void slotAssigned(struct list *l, size_t i) {
  item_free(l->items[i]);
  l->items[i] = NULL;
}

void popSlot(struct list *l) {
  struct item *WEAVEC_OWNED last = l->items[--l->len];
  l->items[l->len] = NULL;
  item_free(last);
}

// stale-borrow
int grown(struct list *l) {
  struct item **first = &l->items[0];
  struct item *WEAVEC_OWNED *bigger = realloc(l->items, 2 * l->cap * sizeof *bigger);
  if (bigger == NULL)
    return -1;
  l->items = bigger;
  l->cap = 2 * l->cap;
  return (*first)->v; // expected-error {{'first' points into what 'l->items' owned before it was released by 'realloc' [weavec-safe:stale-borrow]}}
}

int afterCall(struct list *l) {
  struct item *it = l->items[0];
  list_grow(l);
  return it->v; // expected-error {{'it' was taken from 'l' before the call to 'list_grow', which may release or replace what 'l' owns [weavec-safe:stale-borrow]}}
}

int afterConstCall(struct list *l) {
  struct item *it = l->items[0];
  list_look(l);
  return it->v;
}

int takenAgain(struct list *l) {
  struct item *it = l->items[0];
  list_grow(l);
  it = l->items[0];
  return it->v;
}

// call-alias
void alias(struct list *l) {
  list_put(l, l->items[0]); // expected-error {{'l->items[0]' and 'l' overlap, and 'list_put' may change owners through argument 1 [weavec-safe:call-alias]}}
}

// Found by the conversions: an owner moved to a call and passed to it again;
// a pointer to owners where the parameter does not say so; a local that
// started as a borrow given an allocation.
char *WEAVEC_OWNED append(char *WEAVEC_OWNED s, const char *t);
void edit(char **v, size_t n);
void look(char *const *v, size_t n);
char *WEAVEC_OWNED self(char *WEAVEC_OWNED s) {
  return append(s, s); // expected-error {{'s' is moved to 'append' (argument 1) and passed to it again (argument 2) [weavec-safe:owner-moved]}}
}
void strings(char *WEAVEC_OWNED *WEAVEC_COUNTED_BY(n) v, size_t n) {
  look(v, n);
  edit(v, n); // expected-error {{'v' points to owners and parameter 1 of 'edit' does not say so: the callee could overwrite one [weavec-safe:owner-source]}}
}
int startsBorrowed(int big) {
  char stack[16] = {0};
  char *buf = stack;
  if (big)
    buf = malloc(64); // expected-error {{'malloc' returns an owner and 'buf' is a borrow (it started as one): the block would have no owner [weavec-safe:owner-leak]}}
  return buf[0];
}

// Found by the conversions, accepted: a program's allocator's owned `void *`
// result and a `malloc`-attributed owned result have null owners; a chained
// assignment of null; a null test of an owned parameter before releasing it.
void *WEAVEC_OWNED raw(size_t n) __attribute__((alloc_size(1)));
struct item *WEAVEC_OWNED blank(void) __attribute__((malloc));
struct item *WEAVEC_OWNED made(void) {
  struct item *it = raw(sizeof *it);
  if (it == NULL)
    return NULL;
  it->name = strdup("x");
  return it;
}
void named(void) {
  struct item *it = blank();
  if (it == NULL)
    return;
  it->name = strdup("y");
  item_free(it);
}
struct pair { struct item *WEAVEC_OWNED a, *WEAVEC_OWNED b; };
void cleared(struct pair *p) {
  item_free(p->a);
  item_free(p->b);
  p->a = p->b = NULL;
}
void maybe(char *WEAVEC_OWNED s) {
  if (s != NULL)
    free(s);
}
