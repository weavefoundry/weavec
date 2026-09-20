// RFC 0030 *Diagnostics* (`out-of-bounds`): a string literal has no
// writable byte, so a store through a pointer that points into one on every
// path, or a library call that writes through it, is a definite error. On
// some paths only, the spatial facet is `unresolved(unknown-extent)` and
// nothing is reported.
// RUN: not %weavec --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include <string.h>

void store(void) {
  char *p = "abc";
  // CHECK: rfc0030-literal-write.c:[[@LINE+1]]:3: error: write through 'p', which points to a string literal [weavec::out-of-bounds]
  p[0] = 'x';
}

char *split(void) {
  // CHECK: rfc0030-literal-write.c:[[@LINE+1]]:17: error: write through '"a,b"', which points to a string literal [weavec::out-of-bounds]
  return strtok("a,b", ",");
}

// CHECK-NOT: string literal
void maybe(int c, char *buf) {
  char *p = c ? "abc" : buf;
  p[0] = 'x';
}
// LEDGER: "reason": "unknown-extent",
// LEDGER-NEXT: "detail": "it may point into a string literal",
