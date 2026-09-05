// RFC 0011, *Derived pointers*: a pointer into an object is a copy of its
// base at an offset. A release through a field pointer reaches the summary;
// `container_of` walks back to the allocation; arithmetic that returns to
// the start is a release of the start; anything else is `invalid-release`
// with the offset named.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

#define offsetof(T, f) __builtin_offsetof(T, f)
#define container_of(p, T, f) ((T *)((char *)(p) - offsetof(T, f)))

struct inner { char *buf; };
struct outer { struct inner in; int k; };
struct link { struct link *next; };
struct list { struct link *head; };
struct node { struct link link; int v; };

// -- A field pointer is the object at that field -----------------------------

// The release through `i` is a release of `o->in.buf` in the summary.
// DUMP-LABEL: function 'release_inner':
// DUMP: summary: o->in.buf: freed(free); stores{} returns{} requires{o}
static void release_inner(struct outer *o) {
  struct inner *i = &o->in;
  free(i->buf);
}

void caller(struct outer *o) {
  release_inner(o);
  // CHECK: rfc0011-derived.c:[[@LINE+1]]:7: error: use of 'o->in.buf' after it was freed [weavec::use-after-free]
  use(o->in.buf);
  // CHECK: rfc0011-derived.c:[[@LINE-3]]:3: note: freed here
}

// Freeing the object while a field pointer is still used is a use after
// free through the field pointer (not a conflicting borrow).
void through_field(void) {
  struct outer *o = malloc(sizeof *o);
  if (!o)
    return;
  struct inner *i = &o->in;
  free(o);
  // CHECK: rfc0011-derived.c:[[@LINE+1]]:7: error: use of 'i' after it was freed [weavec::use-after-free]
  use(i);
}

// -- container_of -------------------------------------------------------------

// Freed at minus the offset of `in`: the summary says so, and the caller
// composes it with the field pointer it passed.
// DUMP-LABEL: function 'free_container':
// DUMP: summary: i: freed(free)@-struct outer.in; stores{} returns{}
void free_container(struct inner *i) {
  free(container_of(i, struct outer, in));
}

// Clean: `&o->in` at `+in`, freed at `-in`, is `o` freed at the start.
void use_container(void) {
  struct outer *o = malloc(sizeof *o);
  if (!o)
    return;
  free_container(&o->in);
}

// Clean: the member pointer handed out is the fresh object at the field.
// DUMP-LABEL: function 'make_inner':
// DUMP: summary: stores{} returns{fresh(free) @+struct outer.in extent=16, null}
struct inner *make_inner(void) {
  struct outer *o = malloc(sizeof *o);
  if (!o)
    return 0;
  return &o->in;
}

// Clean: the intrusive list takes `&n->link`, which is `n` at offset zero.
static void push(struct list *l, struct link *WEAVEC_OWNED n) {
  n->next = l->head;
  l->head = n;
}
void link_node(struct list *l) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return;
  push(l, &n->link);
}

// -- Releases away from the start ---------------------------------------------

void field_release(void) {
  struct outer *o = malloc(sizeof *o);
  if (!o)
    return;
  // CHECK: rfc0011-derived.c:[[@LINE+1]]:3: error: 'o' is released but points to field 'in' of its allocation [weavec::invalid-release]
  free(&o->in);
  // CHECK: rfc0011-derived.c:[[@LINE-5]]:21: note: allocated here
}

// Clean: `p - 3` is `s` again, and the summary says `s` is freed at zero.
// DUMP-LABEL: function 'rebase':
// DUMP: summary: s: freed(free); stores{} returns{}
void rebase(char *WEAVEC_OWNED s) {
  char *p = s + 3;
  free(p - 3);
}

// DUMP-LABEL: function 'bad_rebase':
// DUMP: summary: s: freed(free)@+1; stores{} returns{}
void bad_rebase(char *WEAVEC_OWNED s) {
  char *p = s + 3;
  // CHECK: rfc0011-derived.c:[[@LINE+1]]:3: error: 'p' is released but points 1 element past the start of its allocation [weavec::invalid-release]
  free(p - 2);
}

// DUMP-LABEL: function 'stepped':
// DUMP: summary: s: freed(free)@+4; stores{} returns{}
void stepped(int *WEAVEC_OWNED s) {
  s += 4;
  // CHECK: rfc0011-derived.c:[[@LINE+1]]:3: error: 's' is released but points 4 elements past the start of its allocation [weavec::invalid-release]
  free(s);
}

// Clean: the walking pointer is separated from `s` by `!=`-free stepping;
// `s` itself is still at the start.
void walk(char *WEAVEC_OWNED s) {
  char *p = s;
  while (*p)
    p++;
  free(s);
}

// Clean: a loop that steps up and back down leaves the offset unknown, and
// an unknown offset is not reported (RFC 0011, *Accepted false positives*
// lists what is; this is not).
void walk_back(char *WEAVEC_OWNED s, int n) {
  char *p = s;
  for (int i = 0; i < n; i++)
    p++;
  for (int i = 0; i < n; i++)
    p--;
  use(p);
  free(s);
}

// -- Derived pointers and nullness -------------------------------------------

typedef struct frame { int x; int trap; struct frame *next; } frame;
typedef struct state { frame *ci; frame base_ci; } state;

// A callee returning `&L->base_ci` hands out `L` at that field.
// DUMP-LABEL: function 'precall':
// DUMP: summary: stores{} returns{copy L @+struct state.base_ci when[c =0], null when[c positive|negative]} requires{L}
frame *precall(state *L, int c) {
  if (c)
    return NULL;
  return &L->base_ci;
}

// Clean: on some iteration `ci` and `newci` are both `L` at `base_ci`, so
// they are exact aliases there; but the relation is a may-relation after the
// join, and `newci == NULL` says nothing definite about `ci`.
int execute(state *L, frame *ci, int c) {
  int v;
startfunc:
  v = ci->x;
  {
    frame *newci;
    if ((newci = precall(L, c)) == NULL)
      v = ci->trap;
    else {
      ci = newci;
      goto startfunc;
    }
  }
  return v;
}

// A local that equals a caller's place is named by it, not by the derived
// name it may also carry (Lua's `ci = L->ci = next_ci(L)`).
// DUMP-LABEL: function 'next_frame':
// DUMP: summary: L->base_ci.next: read; L->ci: read|written; L->ci->next: read; stores{L->ci = copy L @+struct state.base_ci when[c positive|negative], L->ci = copy L->ci->next} returns{copy L->ci} requires{L}
frame *next_frame(state *L, int c) {
  if (c)
    L->ci = &L->base_ci;
  frame *ci = L->ci = L->ci->next;
  return ci;
}

// -- Freeing an object and the borrows through it ----------------------------

typedef struct hlist { struct hlist *prev; struct hlist *next; } hlist;
typedef struct pair { hlist list; int v; } pair;
typedef struct bucket { hlist *first; hlist *last; } bucket;
typedef struct table { bucket *buckets; hlist list; } table;

// Clean: the bucket array holds pointers into pairs it does not own (they are
// on the table's intrusive list); freeing the array leaves them borrowed by
// nothing that is going away (jansson's `hashtable_do_rehash`).
int rehash(table *t, pair *p) {
  bucket *nb = malloc(sizeof(bucket));
  if (!nb)
    return -1;
  t->buckets->last = &p->list;
  p->list.next = &t->list;
  free(t->buckets);
  t->buckets = nb;
  return 0;
}

// The object's own storage does go with it.
struct cell { int *buf; int n; };
void own(struct cell *c) {
  int *keep = &c->n;
  free(c);
  // CHECK: rfc0011-derived.c:[[@LINE+1]]:4: error: use of 'keep' after it was freed [weavec::use-after-free]
  *keep = 1;
}
