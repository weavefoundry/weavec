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

// The returning ref: a store to the count, the argument copied out.
// DUMP-LABEL: function 'obj_ref':
// DUMP: result path param0 when nonnull
// DUMP: store param0->rc := int
static struct obj *obj_ref(struct obj *o) {
  o->rc++;
  return o;
}

// The unref: a release guarded by the count. The object engine does not infer
// RFC 0010 count functions yet, so it is a possible release of `o` and its
// name (RFC 0031 §5.5; the retired golden comparison of RFC 0030).
// DUMP-LABEL: function 'obj_unref':
// DUMP: release *param0 free may when always
// DUMP: release *param0->name free may when always
static void obj_unref(struct obj *o) {
  if (--o->rc == 0) {
    free(o->name);
    free(o);
  }
}

// The other spellings of the decrement (RFC 0010, *Recognising increments
// and decrements*).
// DUMP-LABEL: function 'unref_post':
// DUMP: release *param0 free may when always
static void unref_post(struct obj *o) {
  if (o->rc-- == 1)
    free(o);
}
// DUMP-LABEL: function 'unref_atomic':
// DUMP: release *param0 free may when always
static void unref_atomic(struct obj *o) {
  if (__atomic_fetch_sub(&o->rc, 1, __ATOMIC_ACQ_REL) == 1)
    free(o);
}
// DUMP-LABEL: function 'unref_sync':
// DUMP: release *param0 free may when always
static void unref_sync(struct obj *o) {
  if (__sync_sub_and_fetch(&o->rc, 1) == 0)
    free(o);
}

// A free not guarded by the count reaching zero is a plain free.
// DUMP-LABEL: function 'not_a_release':
// DUMP: release *param0 free when always
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
// Without RFC 0010 count inference the object engine gives a possible use
// after free here: a warning on correct code (RFC 0031 §5.5;
// the retired golden comparison of RFC 0030, *Lit tests*).
void keep(struct obj *o) { obj_ref(o); }
int retained_then_released(struct obj *o) {
  obj_ref(o);
  obj_unref(o);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: warning: use of 'o' after it may have been freed [weavec::use-after-free]
  return o->rc;
}

// Bugs. The counts below are known where each call is made, so each call is
// analysed with them (RFC 0031 §6.6, *Amendment (numeric contexts)*) and the
// releases are definite frees: the third unref's first access of the freed
// object is its read of `o->rc`, a use before the second release.
int one_release_too_many(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  obj_ref(a);
  obj_unref(a);
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:13: error: use of 'a' after it was freed [weavec::use-after-free]
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE-3]]:3: note: freed here
  return 0;
}

int use_after_last(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  obj_unref(a);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: error: use of 'a' after it was freed [weavec::use-after-free]
  return a->rc;
  // CHECK: rfc0010-refcount.c:[[@LINE-3]]:3: note: freed here
}

// The old engine reported a definite use after the share release. Without
// count inference the object engine reports a possible use after free: an
// error became a warning, the facet is still not proven (RFC 0031 §5.5;
// the retired golden comparison of RFC 0030, *Lit tests*).
int released_borrow(struct obj *o) {
  obj_unref(o);
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: warning: use of 'o' after it may have been freed [weavec::use-after-free]
  return o->rc;
  // CHECK: rfc0010-refcount.c:[[@LINE-3]]:3: note: freed here on some paths
}

int plain_free_kills_shares(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  struct obj *b = obj_ref(a);
  // RFC 0013: obj_new's owned name is visible through the result; the object
  // engine names it by the call that made it (RFC 0031 §5.8, §5.11).
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:3: warning: result of 'obj_new' is leaked [weavec::leak]
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
  // `a` keeps the share `keep` took, and with it `a->name`, which the object
  // engine reports too (the second leak, named by the call that made it).
  // CHECK: rfc0010-refcount.c:[[@LINE+2]]:10: warning: 'a' is leaked [weavec::leak]
  // CHECK: rfc0010-refcount.c:[[@LINE+1]]:10: warning: result of 'obj_new' is leaked [weavec::leak]
  return 0;
}
struct list {
  struct obj *head;
};
// The old engine reported `'p' is leaked` (note: reference taken here) at the
// `obj_ref`. The object engine does not infer the count, so a share taken on
// a borrowed object is not an owned object and its leak is not reported: a
// lost warning (RFC 0031 §5.5; the retired golden comparison of RFC 0030, *Lit tests*).
void local_retained(struct list *l) {
  struct obj *p = l->head;
  obj_ref(p);
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

// CHECK: 5 warnings and 3 errors generated.
