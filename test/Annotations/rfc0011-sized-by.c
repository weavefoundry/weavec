// RFC 0011, *Annotation surface*: WEAVEC_SIZED_BY(n) on a pointer parameter
// gives it `n` elements inside the body and requires that many of every
// caller; a malformed one is `invalid-annotation`.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec %s -- 2>&1 | FileCheck --check-prefix=INVALID %s
// RUN: not %weavec --dump-analysis %s -- 2>/dev/null | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

#if WEAVEC_H_VERSION_MINOR < 6
#error "weavec.h 0.6 spells WEAVEC_SIZED_BY"
#endif

// Inside the body the extent is `n` bytes; the access at `n` is one past.
// RFC 0030 §3.3: a declared count is a lower bound on the object, so that
// access is a checked facet, not an error.
// RFC 0017 retains body requirements for callers and wrappers, even when
// the loop was proved against the annotated extent inside this function.
// DUMP-LABEL: function 'fill':
// DUMP: spatial: proven=1 violation=1 unresolved=0
// DUMP-NEXT: summary: *p: written; stores{} returns{} requires{p} requires-extent{p: n when[n positive|negative], p: n+1 start n}
void fill(char *WEAVEC_SIZED_BY(n) p, size_t n) {
  for (size_t i = 0; i < n; i++)
    p[i] = 0;
  p[n] = 0;
}

// Elements, not bytes: `n` ints.
// DUMP-LABEL: function 'ints':
// DUMP: summary: *p: written; stores{} returns{} requires{p} requires-extent{p: (n-1)*4+4 start (n-1)*4}
void ints(int *WEAVEC_SIZED_BY(n) p, int n) { p[n - 1] = 0; }

// The annotation is authoritative for a prototype with no body in view; it
// composes with the ownership annotation the boundary needs (RFC 0003).
void library_fill(char *WEAVEC_MUT WEAVEC_SIZED_BY(len) buf, size_t len);

void calls(void) {
  char buf[4];
  fill(buf, 4);
  // CHECK: rfc0011-sized-by.c:[[@LINE+1]]:8: error: 'fill' requires 8 bytes behind 'buf', which has 4 bytes [weavec::out-of-bounds]
  fill(buf, 8);
  library_fill(buf, 4);
  // CHECK: rfc0011-sized-by.c:[[@LINE+1]]:16: error: 'library_fill' requires 5 bytes behind 'buf', which has 4 bytes [weavec::out-of-bounds]
  library_fill(buf, 5);
  int four[4];
  ints(four, 4);
  // CHECK: rfc0011-sized-by.c:[[@LINE+1]]:8: error: 'ints' requires 20 bytes behind 'four', which has 16 bytes [weavec::out-of-bounds]
  ints(four, 5);
}

// -- Malformed ---------------------------------------------------------------
// RFC 0030 §7.2: `AttributeReader` reports these before the engine runs,
// in its wording, and drops the kind.

// INVALID: rfc0011-sized-by.c:[[@LINE+1]]:41: warning: 'x' is declared WEAVEC_SIZED_BY(n) but is not a pointer [weavec::invalid-annotation]
void not_pointer(int WEAVEC_SIZED_BY(n) x, int n) { (void)x; (void)n; }
// INVALID: rfc0011-sized-by.c:[[@LINE+1]]:43: warning: 'q' in WEAVEC_SIZED_BY is not an integer parameter or field [weavec::invalid-annotation]
void not_counter(char *WEAVEC_SIZED_BY(q) p, char *q) { (void)p; (void)q; }
// INVALID: rfc0011-sized-by.c:[[@LINE+1]]:39: warning: 'm' in WEAVEC_SIZED_BY does not name a parameter or field [weavec::invalid-annotation]
void no_such(char *WEAVEC_SIZED_BY(m) p, int n) { (void)p; (void)n; }
