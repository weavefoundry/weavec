// RFC 0010, *Per-outcome stores* and *Per-outcome integer facts*: a store a
// callee makes only on some outcome classes is retracted on the others, and
// what the callee left in the caller's integer memory per class is known
// after the call, which lets a wrapped decrement release a share.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

// -- Per-outcome stores ------------------------------------------------------

struct bag {
  char *items[8];
  int n;
};

// The store happens on the zero class only; the negative class leaves the
// caller's memory alone and carries the test that failed.
// RFC 0017: the requirement uses entry n, before the postfix increment. The
// eight-byte slot starts at n*8 and ends at n*8+8; the typed guard excludes 8.
// The object engine exports the store through the variable index `b->n` as a
// possible store to some element, without the result class that makes it
// (RFC 0031 §4.9, *Summaries*: a range whose bounds cannot be expressed is
// exported as a possible effect on some elements); a store at a constant
// index keeps `when result zero`.
// DUMP-LABEL: function 'bag_put':
// DUMP: result int [-1, -1] when negative
// DUMP: result int [0, 0] when zero
// DUMP: store param0->items[*] := path param1 may
static int bag_put(struct bag *b, char *s) {
  if (b->n == 8)
    return -1;
  b->items[b->n++] = s;
  return 0;
}

// Clean: on the failure edge the bag does not hold `s`, so freeing it is
// this function's job; on success it escaped into the bag.
int put_or_free(struct bag *b) {
  char *s = malloc(8);
  if (!s)
    return -1;
  if (bag_put(b, s) < 0) {
    free(s);
    return -1;
  }
  return 0;
}

// The failure edge without the free: `s` is still this function's.
int put_and_forget(struct bag *b) {
  char *s = malloc(8);
  if (!s)
    return -1;
  // The old engine reported `'s' is leaked` on the failure edge below. The
  // object engine's summary of `bag_put` stores `s` into the bag possibly on
  // every class (above), so on that edge `s` may have escaped and no leak is
  // reported: a lost warning, never a proof (the retired golden comparison of RFC 0030,
  // *Lit tests*).
  if (bag_put(b, s) < 0)
    return -1;
  return 0;
}

// -- Per-outcome integer facts -----------------------------------------------

struct obj {
  int rc;
};

// The wrapped decrement: `*r` is zero exactly on the positive class. The
// comparison splits the exit by its truth (RFC 0031 *Pending cases and exit
// splitting*); the written count is exported as an interval, not per class
// (RFC 0031 §6.1 drops the per-outcome integer facts of format 29).
// DUMP-LABEL: function 'dec_and_test':
// DUMP: result int [0, 0] when zero and param 0 !=0
// DUMP: result int [1, 1] when positive and param 0 !=0
// DUMP: store *param0 := int [-2147483648, 2147483647]
static int dec_and_test(int *r) { return --*r == 0; }

// Through the helper, the unref releases `o` when the count reaches zero.
// The object engine does not infer RFC 0010 count functions yet, so it is a
// possible release (RFC 0031 §5.5; the retired golden comparison of RFC 0030).
// DUMP-LABEL: function 'obj_unref':
// DUMP: release *param0 free may when always
// DUMP: store param0->rc := int
static void obj_unref(struct obj *o) {
  if (dec_and_test(&o->rc))
    free(o);
}

static struct obj *obj_new(void) {
  struct obj *o = malloc(sizeof *o);
  if (!o)
    return NULL;
  o->rc = 1;
  return o;
}

int twice(void) {
  struct obj *a = obj_new();
  if (!a)
    return -1;
  obj_unref(a);
  // `a->rc` is 1, so the first call frees `a`; the second call's first access
  // is the read of `o->rc` in `dec_and_test`, a use of the freed object before
  // its second release (RFC 0031 §6.6, *Amendment (numeric contexts)*: the
  // call is analysed with the count the caller knows; with an unknown count
  // the finding is a warning).
  // CHECK: rfc0010-outcomes.c:[[@LINE+1]]:13: error: use of 'a' after it was freed [weavec::use-after-free]
  obj_unref(a);
  return 0;
}

// A caller learns the callee's facts on the edge it takes.
struct box {
  int filled;
  char *p;
};
// DUMP-LABEL: function 'fill':
// DUMP: result int [-1, -1] when negative
// DUMP: result int [0, 0] when zero
// DUMP: store param0->p := path param1 when result zero
static int fill(struct box *b, char *p) {
  if (!p) {
    b->filled = 0;
    return -1;
  }
  b->p = p;
  b->filled = 1;
  return 0;
}

// Clean: on the failure edge `b->filled` is zero, so the use is unreachable
// and `p` was never stored.
void consumer(struct box *b) {
  char *p = malloc(8);
  if (fill(b, p) < 0) {
    if (b->filled)
      use(b->p);
    free(p);
  }
}

// CHECK: 1 error generated.
