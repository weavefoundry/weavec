// RFC 0041 §3: every error carries a note with the accepted form. RFC 0049
// §2: when the compiler can write the form, the note says the text at the
// declaration; when it cannot, it gives the rule's form.
// RUN: not %weavec_cc -fsyntax-only -fno-caret-diagnostics %s 2>&1 | FileCheck %s
#include <weavec.h>

WEAVEC_SAFE int first(const int *p) {
  // CHECK: notes.c:[[@LINE+2]]:{{[0-9]+}}: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
  // CHECK-NEXT: notes.c:[[@LINE-2]]:{{[0-9]+}}: note: fix (likely): declare it 'const int *WEAVEC_COUNTED_BY(2) p'; only its elements 0 to 1 are read
  return p[1];
}

WEAVEC_SAFE int second(const int *p, int i) {
  // CHECK: notes.c:[[@LINE+2]]:{{[0-9]+}}: error: 'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]
  // CHECK-NEXT: notes.c:[[@LINE+1]]:{{[0-9]+}}: note: give its declaration a bound: 'T *WEAVEC_COUNTED_BY(n) p'
  return p[i];
}
// CHECK: note: 1 of 2 errors carry a fix; 'weavec-cc --weavec-fix -- <this command>' applies them
