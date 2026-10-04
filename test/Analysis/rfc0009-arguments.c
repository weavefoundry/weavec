// RFC 0009, *Deriving guards* and *Return alternatives*: a callee's consume,
// store or return that happens only under a fact about its parameters is
// summarised with a `when` guard; the caller translates the guard to its
// arguments and applies the effect only when its own facts do not refute it.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

struct buf {
  char *data;
  int noalloc;
};

struct state {
  char *msg;
  int err;
};

// Lua's `l_alloc`: both arms consume `ptr` (`free` or `realloc`), so that
// effect is unconditional; only a non-zero size yields a fresh block. Each
// outcome class keeps its guard (RFC 0009, *Guards*): the null class frees
// `ptr` only for a zero size, so a caller that tests the result and knows
// the size is non-zero still owns the block.
// Format 30 (RFC 0031 §6.1) keys each effect by the result class and the
// size parameter's zero test.
// DUMP-LABEL: function 'l_alloc':
// DUMP: result null when null
// DUMP-NEXT: result fresh#0 free extent param3 {{.*}}when nonnull and param 3 !=0
// DUMP-NEXT: release *param1 free when result null and param 3 =0
// DUMP-NEXT: move *param1 free when result nonnull
void *l_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  (void)ud;
  (void)osize;
  if (nsize == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, nsize);
}

// Lua's `luaS_resize` shape: on failure the table is left as it was, which
// is a dangling `hash` only when the size was zero (the block was freed).
// The summary text joins the size-zero release into the move and keeps the
// replacement a possible store (RFC 0031 *Implementation amendments*, *Stores
// that keep the entry value possible*); `nsize * 8` is no parameter test.
// DUMP-LABEL: function 'resize_table':
// DUMP: move *param1->hash free when always
// DUMP-NEXT: store param1->hash := fresh#0 free {{.*}}may
struct table {
  void **hash;
  int size;
};
void resize_table(void *ud, struct table *t, int nsize) {
  void **nv = l_alloc(ud, t->hash, 8, nsize * sizeof(void *));
  if (nv == NULL) {
    /* leave the table as it was */
  } else {
    t->hash = nv;
    t->size = nsize;
  }
}

// cJSON's `printbuffer` shape: the free depends on a flag in the object.
// The flag is no result class or parameter test, so the release is possible
// in the summary text; a caller whose record decides the flag gets the exact
// effect (`keep_static`, `free_heap`).
// DUMP-LABEL: function 'release':
// DUMP: release *param0->data free may when always
void release(struct buf *b) {
  if (!b->noalloc)
    free(b->data);
}

// zlib's `gz_error`: the store depends on the argument being non-null.
// DUMP-LABEL: function 'gz_error':
// DUMP: store param0->msg := path param2 when param 2 !=0
void gz_error(struct state *s, int err, char *msg) {
  s->err = err;
  if (msg != NULL)
    s->msg = msg;
}

// Clean callers: the argument decides.

void grow(void *ud) {
  char *p = malloc(8);
  char *q = l_alloc(ud, p, 8, 16);
  use(q);
  free(q);
}

void known_size(void *ud) {
  size_t n = 32;
  char *p = malloc(8);
  char *q = l_alloc(ud, p, 8, n);
  use(q);
  free(q);
}

void scaled_size(void *ud, int count) {
  char *p = malloc(8);
  if (count > 0) {
    char *q = l_alloc(ud, p, 8, count * 8);
    use(q);
    free(q);
  } else {
    free(p);
  }
}

// The null edge frees `t->hash` only for a zero size, which this caller
// refuted: returning is neither a leak nor leaves a freed value behind.
void grow_or_keep(void *ud, struct table *t) {
  if (t->size == 0)
    return;
  void **nv = l_alloc(ud, t->hash, 8, t->size * 2 * sizeof(void *));
  if (nv == NULL)
    return;
  t->hash = nv;
}

void keep_static(void) {
  char stack[8];
  struct buf b;
  b.data = stack;
  b.noalloc = 1;
  release(&b);
  use(b.data);
}

void store_null(struct state *s) {
  gz_error(s, 1, NULL);
}

// Reported callers.

// The discarded result is null here, not a leak; the block itself is gone.
// A zero size decides `l_alloc`'s parameter test, so the release is certain
// (RFC 0031 *Implementation amendments*, *Pending cases and exit splitting*).
void shrink(void *ud) {
  char *p = malloc(8);
  l_alloc(ud, p, 8, 0);
  // CHECK: rfc0009-arguments.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
}

void unknown_size(void *ud, size_t n) {
  char *p = malloc(8);
  char *q = l_alloc(ud, p, 8, n);
  // Freed for a zero size, moved when `realloc` succeeds, live when it fails:
  // the untested result names the move of the non-null class (RFC 0031 §6.1).
  // CHECK: rfc0009-arguments.c:[[@LINE+1]]:7: warning: use of 'p' after it may have been moved [weavec::use-after-move]
  use(p);
  free(q);
}

void free_heap(void) {
  struct buf b;
  b.data = malloc(8);
  b.noalloc = 0;
  release(&b);
  // CHECK: rfc0009-arguments.c:[[@LINE+1]]:7: error: use of 'b.data' after it was freed [weavec::use-after-free]
  use(b.data);
}

void unknown_flag(struct buf *b) {
  release(b);
  // CHECK: rfc0009-arguments.c:[[@LINE+1]]:7: warning: use of 'b->data' after it may have been freed [weavec::use-after-free]
  use(b->data);
}

void store_local(struct state *s) {
  char local[8];
  // RFC 0033 §1: storing the address is no access; a warning.
  // CHECK: rfc0009-arguments.c:[[@LINE+1]]:3: warning: 's->msg' may outlive 'local', which it points to [weavec::lifetime-too-short]
  gz_error(s, 1, local);
}

// CHECK: 3 warnings and 2 errors generated.
