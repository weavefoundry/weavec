// RFC 0010, *Stores out of sight*: a value copied into a node the callee
// allocates and links into the caller's container has a second home the
// summary cannot name; the callee says `escaped`, wrappers pass it on, and
// the caller reports neither the value nor the share it just took as leaked.
// RUN: %weavec %s -- 2>&1 | FileCheck %s
// RUN: %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

struct obj {
  int rc;
};

static struct obj *obj_ref(struct obj *o) {
  o->rc++;
  return o;
}

static void obj_unref(struct obj *o) {
  if (--o->rc == 0)
    free(o);
}

struct pair {
  struct pair *next;
  struct obj *value;
};

struct table {
  struct pair *first;
};

// `p->value = value` has no caller-visible destination; `value` escapes, and
// so does the old head, which lives on in the new node's `next`.
// DUMP-LABEL: function 'table_set':
// DUMP: summary: t->first: read|written|escaped; value: escaped; stores{t->first = fresh(free) extent=16}
static int table_set(struct table *t, struct obj *value) {
  struct pair *p = malloc(sizeof *p);
  if (!p)
    return -1;
  p->value = value;
  p->next = t->first;
  t->first = p;
  return 0;
}

// The wrapper that releases on failure: `escaped` and the conditional share
// release both reach the caller.
// DUMP-LABEL: function 'table_set_new':
// DUMP: summary: t->first: written|escaped; value: freed(free),share|escaped;
static int table_set_new(struct table *t, struct obj *value) {
  if (!value)
    return -1;
  if (table_set(t, value)) {
    obj_unref(value);
    return -1;
  }
  return 0;
}

// The share-taking wrapper: `obj_ref(value)` is `value` or null, so `param 1`
// of the callee resolves to `value` and its increment, decrement and escape
// compose.
// DUMP-LABEL: function 'table_set_shared':
// DUMP: summary: t->first: written|escaped; value: escaped; value->rc: written;
// DUMP-SAME: increments{value->rc} decrements{value->rc}
static int table_set_shared(struct table *t, struct obj *value) {
  return table_set_new(t, obj_ref(value));
}

struct obj *iter_value(void *it);

// Clean: the node keeps the owned object.
int owned_into_table(struct table *t) {
  struct obj *o = malloc(sizeof *o);
  if (!o)
    return -1;
  o->rc = 1;
  return table_set(t, o);
}

// Clean: the share the wrapper took went into the node.
int shares_into_table(struct table *t, void *it) {
  struct obj *value;
  while ((value = iter_value(it)) != NULL) {
    if (table_set_shared(t, value))
      return -1;
  }
  return 0;
}

// A node on the stack dies with the call: no escape.
// DUMP-LABEL: function 'use_locally':
// DUMP-NOT: escaped
// DUMP-LABEL: function 'still_leaks':
struct ctx {
  struct obj *o;
};
int use_locally(struct obj *o) {
  struct ctx c;
  struct ctx *p = &c;
  p->o = o;
  return p->o->rc;
}

int still_leaks(void) {
  struct obj *a = malloc(sizeof *a);
  if (!a)
    return -1;
  // CHECK: rfc0010-escapes.c:[[@LINE+1]]:3: warning: 'a' is leaked [weavec::leak]
  return use_locally(a);
}
