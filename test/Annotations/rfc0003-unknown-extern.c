// RFC 0003: a call to a function with neither a definition in this
// translation unit nor ownership annotations is a checking boundary. RFC 0030
// §5.1 makes it an unknown callee: no diagnostic, a Call site whose temporal
// facet is `unresolved(unknown-callee)` with a suggestion (a fix-it when the
// declaration is the program's own), and what it may have freed or kept is
// unresolved after it. A third-party header installed as a system header is
// no platform header (§5.2), so its functions are unknown too. RFC 0030
// removes `annotation-required` and `--strict-externs`.
// RUN: %weavec --ledger=%t.json %s -- -isystem %S/Inputs 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include "../Inputs/prelude.h"
#include <weavec.h>
#include <vendor.h>

void mystery(void *p);
int pure(int x);
void *maker(void);
void annotated(void *WEAVEC_BORROWED p);

void f(char *p) {
  // LEDGER: "text": "mystery(p)",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "declare 'mystery' with WEAVEC_BORROWED on 'p' if it neither keeps nor frees it",
  // LEDGER-NEXT: "fixit": {
  // LEDGER-NEXT: "file": "{{.*}}rfc0003-unknown-extern.c",
  // LEDGER-NEXT: "line": 15,
  // LEDGER-NEXT: "column": 20,
  // LEDGER-NEXT: "insertion": "WEAVEC_BORROWED "
  mystery(p);
  // LEDGER: "text": "mystery(p)",
  // LEDGER: "reason": "unknown-callee",
  mystery(p);
  // LEDGER: "text": "pure(1)",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "define 'pure' in this program, or link a unit that has its WeaveC record",
  pure(1);
  // `annotated` only borrows `p`, but `mystery` may have freed it.
  // LEDGER: "text": "annotated(p)",
  // LEDGER: "reason": "unknown-callee",
  annotated(p);
  // No fix-it into a system header; the detail names the first unknown code
  // that may have freed `p` (`mystery`).
  // LEDGER: "text": "vendor_touch(p)",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "mystery",
  // LEDGER-NEXT: "fixit": null,
  vendor_touch(p);
  // LEDGER: "text": "use(maker())",
  // LEDGER: "reason": "extern-contract",
  // LEDGER: "text": "maker()",
  // LEDGER: "reason": "unknown-callee",
  // LEDGER-NEXT: "detail": "declare the result of 'maker' WEAVEC_OWNED or WEAVEC_BORROWED, or define 'maker' in this program",
  use(maker());
}

// An unsafe region does not make a callee known.
void vouched(char *p) {
  // LEDGER: "text": "mystery(p)",
  // LEDGER: "reason": "unknown-callee",
  WEAVEC_UNSAFE { mystery(p); }
}

// CHECK-NOT: warning:
// CHECK: 0 errors, 0 warnings
// LEDGER: "diagnostics": []
