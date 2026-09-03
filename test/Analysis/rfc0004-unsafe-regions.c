// RFC 0004, "Unsafe regions": a WEAVEC_UNSAFE block or function body is
// analysed, its raw operations are permitted and nothing inside it is
// reported, but its effects flow out and are checked in the code around it.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

typedef unsigned long uintptr_t;
struct node {
  int v;
  struct node *next;
};

// Raw operations inside the region are fine.
void permitted(uintptr_t x) {
  WEAVEC_UNSAFE {
    struct node *n = (struct node *)x;
    n->v = 1;
    free(n);
  }
}

WEAVEC_UNSAFE void whole_function(struct node *WEAVEC_RAW r) {
  r->v = 1;
  free(r);
  free(r); /* wrong, but the author has taken responsibility */
}

// Nothing inside is reported ...
void suppressed(struct node *p) {
  free(p);
  WEAVEC_UNSAFE {
    use(p);
    free(p);
  }
}

// ... but a free inside is a free, seen outside.
void escapes(struct node *p) {
  WEAVEC_UNSAFE { free(p); }
  // CHECK: rfc0004-unsafe-regions.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
  // CHECK: rfc0004-unsafe-regions.c:[[@LINE-3]]:19: note: freed here
}

// A raw value made inside stays raw outside.
void raw_escapes(uintptr_t x) {
  struct node *n;
  WEAVEC_UNSAFE { n = (struct node *)x; n->v = 1; }
  // CHECK: rfc0004-unsafe-regions.c:[[@LINE+1]]:3: error: dereference of raw pointer 'n' outside an unsafe region [weavec::unsafe-operation]
  n->v = 2;
}

// The unsafe function's summary is consulted by its callers like any other.
WEAVEC_UNSAFE static void release(struct node *n) { free(n); }
void caller(struct node *n) {
  release(n);
  // CHECK: rfc0004-unsafe-regions.c:[[@LINE+1]]:7: error: use of 'n' after it was freed [weavec::use-after-free]
  use(n);
}

// A bodyless WEAVEC_UNSAFE declaration still means "trust me, no effects"
// (RFC 0003).
WEAVEC_UNSAFE void vouched(void *p);
void trusts(struct node *n) {
  vouched(n);
  use(n);
}

// A nested block is just part of the region.
void nested(uintptr_t x) {
  WEAVEC_UNSAFE {
    struct node *n = (struct node *)x;
    if (n) {
      n->v = 1;
    }
  }
}

// CHECK: 3 errors generated.

// The dump shows the raw component of the state and a `raw` kind.
// DUMP-LABEL: function 'whole_function' (unsafe):
// DUMP-NEXT: places: r (param, raw)
// DUMP: exit: moved{r@[[@LINE-60]]:3 freed(free)} loans{} aliases{} raw{r@[[@LINE-62]]:{{[0-9]+}} declared} owned{}
// DUMP-LABEL: function 'raw_escapes':
// DUMP-NEXT: places: x (param) n (local, raw)
