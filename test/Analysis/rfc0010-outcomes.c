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
// DUMP-LABEL: function 'bag_put':
// DUMP: summary: b->items[*]: written; b->n: read|written; stores{b->items[*] = copy s} returns{} requires{b} outcome zero{} stored{b->items[*]} outcome negative{} stored{} facts{b->n =8} increments{b->n}
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
  if (bag_put(b, s) < 0)
    // CHECK: rfc0010-outcomes.c:[[@LINE+1]]:5: warning: 's' is leaked [weavec::leak]
    return -1;
  return 0;
}

// -- Per-outcome integer facts -----------------------------------------------

struct obj {
  int rc;
};

// The wrapped decrement: `*r` is zero exactly on the positive class.
// DUMP-LABEL: function 'dec_and_test':
// DUMP: summary: *r: read|written; stores{} returns{} requires{r} outcome zero{} facts{*r positive|negative} outcome positive{} facts{*r =0} decrements{*r}
static int dec_and_test(int *r) { return --*r == 0; }

// Through the helper, the unref is still a share release through `o->rc`.
// DUMP-LABEL: function 'obj_unref':
// DUMP: summary: o: freed(free),share; o->rc: written; stores{} returns{} requires{o} decrements{o->rc} counts{o->rc}
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
  // CHECK: rfc0010-outcomes.c:[[@LINE+1]]:3: error: 'a' is released twice [weavec::double-free]
  obj_unref(a);
  return 0;
}

// A caller learns the callee's facts on the edge it takes.
struct box {
  int filled;
  char *p;
};
// DUMP-LABEL: function 'fill':
// DUMP: summary: b->filled: written; b->p: written; stores{b->p = copy p when[p nonnull]} returns{} requires{b} outcome zero{} notnull{b->p} stored{b->p} facts{b->filled =1} outcome negative{} stored{} facts{b->filled =0}
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

// CHECK: 1 warning and 1 error generated.
