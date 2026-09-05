// RFC 0011, *Annotation surface*: WEAVEC_SIZED_BY(n) on a pointer parameter
// gives it `n` elements inside the body and requires that many of every
// caller; a malformed one is `invalid-annotation`.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --dump-analysis %s -- 2>&1 | FileCheck --check-prefix=DUMP %s
#include "../Inputs/prelude.h"
#include <weavec.h>

#if WEAVEC_H_VERSION_MINOR < 6
#error "weavec.h 0.6 spells WEAVEC_SIZED_BY"
#endif

// Inside the body the extent is `n` bytes; the access at `n` is one past.
// The requirement comes from the annotation, so the inferred summary shows
// none of its own (the loop is proved against the declared extent).
// DUMP-LABEL: function 'fill':
// DUMP: summary: *p: written; stores{} returns{} requires{p}
// DUMP-NOT: requires-extent
void fill(char *WEAVEC_SIZED_BY(n) p, size_t n) {
  for (size_t i = 0; i < n; i++)
    p[i] = 0;
  // CHECK: rfc0011-sized-by.c:[[@LINE+1]]:3: error: 'p[n]' is out of bounds: 'n' is the number of elements of 'p' [weavec::out-of-bounds]
  p[n] = 0;
  // CHECK: rfc0011-sized-by.c:[[@LINE-5]]:36: note: 'p' is declared here
}

// Elements, not bytes: `n` ints.
// DUMP-LABEL: function 'ints':
// DUMP: summary: *p: written; stores{} returns{} requires{p}
// DUMP-NOT: requires-extent
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

// CHECK: rfc0011-sized-by.c:[[@LINE+1]]:41: warning: 'x' is declared WEAVEC_SIZED_BY(n) but is not a pointer [weavec::invalid-annotation]
void not_pointer(int WEAVEC_SIZED_BY(n) x, int n) { (void)x; (void)n; }
// CHECK: rfc0011-sized-by.c:[[@LINE+1]]:43: warning: 'p' is declared WEAVEC_SIZED_BY(q) but 'q' is not an integer parameter [weavec::invalid-annotation]
void not_counter(char *WEAVEC_SIZED_BY(q) p, char *q) { (void)p; (void)q; }
// CHECK: rfc0011-sized-by.c:[[@LINE+1]]:39: warning: 'p' is declared WEAVEC_SIZED_BY(m) but 'm' is not an integer parameter [weavec::invalid-annotation]
void no_such(char *WEAVEC_SIZED_BY(m) p, int n) { (void)p; (void)n; }
