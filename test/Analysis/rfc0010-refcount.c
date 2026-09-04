// RFC 0010, *Inferring reference counts*, *Shares* and *Bugs caught*: a
// count increment retains a share on the holder, a zero-guarded free after
// a decrement releases one, copies of a holder with a surplus take their own
// share, and a plain free kills every name.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

struct obj {
  int rc;
  char *name;
};

static struct obj *obj_new(void) {
  struct obj *o = malloc(sizeof *o);
  if (!o)
    return NULL;
  o->rc = 1;
  o->name = malloc(4);
  return o;
}

// The returning ref: `increment` on the count, the argument copied out.
// DUMP-LABEL: function 'obj_ref':
// DUMP: summary: o->rc: read|written; stores{} returns{copy o} requires{o} increments{o->rc}
static struct obj *obj_ref(struct obj *o) {
  o->rc++;
  return o;
}

// The unref: a share release, spelled `freed(free),share`, and the count it
// releases through.
// DUMP-LABEL: function 'obj_unref':
// DUMP: summary: o: freed(free),share; o->rc: read|written; stores{} returns{} requires{o} decrements{o->rc} counts{o->rc}
static void obj_unref(struct obj *o) {
  if (--o->rc == 0) {
    free(o->name);
    free(o);
  }
}

// The other spellings of the decrement (RFC 0010, *Recognising increments
// and decrements*).
// DUMP-LABEL: function 'unref_post':
// DUMP: summary: o: freed(free),share; o->rc: read|written; stores{} returns{} requires{o} decrements{o->rc} counts{o->rc}
static void unref_post(struct obj *o) {
  if (o->rc-- == 1)
    free(o);
}
// DUMP-LABEL: function 'unref_atomic':
// DUMP: summary: o: freed(free),share; o->rc: read|written; stores{} returns{} requires{o} decrements{o->rc} counts{o->rc}
static void unref_atomic(struct obj *o) {
  if (__atomic_fetch_sub(&o->rc, 1, __ATOMIC_ACQ_REL) == 1)
    free(o);
}
// DUMP-LABEL: function 'unref_sync':
// DUMP: summary: o: freed(free),share; o->rc: read|written; stores{} returns{} requires{o} decrements{o->rc} counts{o->rc}
static void unref_sync(struct obj *o) {
  if (__sync_sub_and_fetch(&o->rc, 1) == 0)
    free(o);
}

// A free not guarded by the count reaching zero is a plain free.
// DUMP-LABEL: function 'not_a_release':
// DUMP: summary: o: freed(free); o->rc: read|written; stores{} returns{} requires{o} decrements{o->rc}
static void not_a_release(struct obj *o) {
  o->rc--;
  free(o);
}

// Clean: a share taken and given back, through a copy or the holder itself.
int balanced(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  struct obj *b = obj_ref(a);
  obj_unref(b);
  use(a->name);
  obj_ref(a);
  obj_unref(a);
  use(a->name);
  obj_unref(a);
  return 0;
}

// Clean: a stored copy carries the surplus share; releasing the source
// leaves it.
struct holder {
  struct obj *o;
};
int stored_share(struct holder *h) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  h->o = obj_ref(a);
  obj_unref(a);
  return h->o->rc;
}

// Clean: a share retained on a parameter is the caller's business; a
// release of a share this function does not own is a discipline.
void keep(struct obj *o) { obj_ref(o); }
int retained_then_released(struct obj *o) {
  obj_ref(o);
  obj_unref(o);
  return o->rc;
}

// Bugs.
int one_release_too_many(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  obj_ref(a);
  obj_unref(a);
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:3: error: 'a' is released twice [weavec::double-free]
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE-3]]:3: note: previously released here
  return 0;
}

int use_after_last(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: error: use of 'a' after its reference was released [weavec::use-after-free]
  return a->rc;
  // CHECK: rfc0010-refcount.c:[[@LINE-3]]:3: note: reference released here
}

int released_borrow(struct obj *o) {
  obj_unref(o);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: error: use of 'o' after its reference was released [weavec::use-after-free]
  return o->rc;
}

int plain_free_kills_shares(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  struct obj *b = obj_ref(a);
  free(a);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: error: use of 'b' after it was freed [weavec::use-after-free]
  return b->rc;
  // CHECK: rfc0010-refcount.c:[[@LINE-3]]:3: note: freed here (through 'a')
}

// Leaks of shares (RFC 0010, *Leaks of shares*): the caller of `keep` loses
// the share it took; a local retained and dropped is a leak because
// `struct obj.rc` is a known count (`obj_unref` releases through it).
int caller_loses_share(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  keep(a);
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: warning: 'a' is leaked [weavec::leak]
  return 0;
}
struct list {
  struct obj *head;
};
void local_retained(struct list *l) {
  struct obj *p = l->head;
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:3: warning: 'p' is leaked [weavec::leak]
  obj_ref(p);
  // CHECK: rfc0010-refcount.c:[[@LINE-1]]:3: note: reference taken here
}

// A field nobody releases through is not a count: no leak.
struct sized {
  int len;
  char *buf;
};
void not_a_count(struct sized *s) {
  struct sized *t = s;
  t->len++;
}

// CHECK: 2 warnings and 4 errors generated.
