// RFC 0041 §3: every error carries a note with the accepted form.
// RUN: not %weavec_cc -fsyntax-only -fno-caret-diagnostics %s 2>&1 | FileCheck %s
#include <weavec.h>

WEAVEC_SAFE int first(const int *p) {
  // CHECK: notes.c:[[@LINE+2]]:{{[0-9]+}}: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
  // CHECK-NEXT: notes.c:[[@LINE+1]]:{{[0-9]+}}: note: give its declaration a bound: 'T *WEAVEC_COUNTED_BY(n) p'
  return p[1];
}
