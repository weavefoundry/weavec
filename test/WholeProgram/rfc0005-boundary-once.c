// RFC 0005 reported `annotation-required` once per program for a callee no
// unit defines. RFC 0030 §5.1 removes the warning: every call to such a
// callee is an `unresolved(unknown-callee)` ledger row with a suggestion, and
// a callee another unit of the program defines is known to the whole-program
// analysis (in a per-unit analysis it is unknown too).
//
// RUN: %weavec --whole-program %s %S/Inputs/node.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: %weavec --ledger=%t.json %s -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=ALONE %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include "../Inputs/prelude.h"
#include "node.h"

struct blob;
struct blob *blob_open(const char *path);
void blob_close(struct blob *b);

// CHECK-NOT: warning:
// Whole program: only the four calls to the `blob_` functions.
// CHECK: rfc0005-boundary-once.c: 9 sites: 5 proven, 0 checkable (not enforced), 4 unresolved, 0 trusted; 0 errors, 0 warnings
// ALONE-NOT: warning:
// ALONE: rfc0005-boundary-once.c: 9 sites: 3 proven, 0 checkable (not enforced), 6 unresolved, 0 trusted; 0 errors, 0 warnings

void first_caller(void) {
  // LEDGER: "text": "blob_open(\"x\")",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "declare 'blob_open' with WEAVEC_BORROWED on 'path' if it neither keeps nor frees it",
  // LEDGER-NEXT: "fixit": {
  // LEDGER-NEXT: "file": "{{.*}}rfc0005-boundary-once.c",
  // LEDGER-NEXT: "line": 14,
  struct blob *b = blob_open("x");
  // LEDGER: "text": "blob_close(b)",
  // LEDGER: "reason": "unknown-callee",
  blob_close(b);
}

void second_caller(void) {
  // LEDGER: "text": "blob_open(\"y\")",
  // LEDGER: "reason": "unknown-callee",
  struct blob *b = blob_open("y");
  // LEDGER: "text": "blob_close(b)",
  // LEDGER: "reason": "unknown-callee",
  blob_close(b);
}

// Defined in node.c: unknown to this unit alone.
void uses_the_library(void) {
  // LEDGER: "text": "node_new()",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "declare the result of 'node_new' WEAVEC_OWNED or WEAVEC_BORROWED, or define 'node_new' in this program",
  struct node *n = node_new();
  node_free(n);
}
