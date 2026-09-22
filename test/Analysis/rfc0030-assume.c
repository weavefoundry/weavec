// RFC 0030 §6.2: an Assume site's assertion facet is proven when the analysis
// proves `e` there, a violation (the `contradicted-assumption` error) when it
// refutes `e`, and checked otherwise; `e` holds after the site in every case.
// RUN: not %weavec --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
#include <stddef.h>
#include <weavec.h>

int proven(int n) {
  if (n <= 0)
    return 0;
  // LEDGER: "text": "WEAVEC_ASSUME(n>0)",
  // LEDGER: "assertion": {
  // LEDGER-NEXT: "outcome": "proven",
  WEAVEC_ASSUME(n > 0);
  return n;
}

int checked(char *b, int n) {
  // LEDGER: "text": "WEAVEC_ASSUME(n>0)",
  // LEDGER: "assertion": {
  // LEDGER-NEXT: "outcome": "checked",
  WEAVEC_ASSUME(n > 0);
  return b[n - 1];
}

size_t last(const char *s) {
  size_t len = 0;
  (void)s;
  // CHECK: rfc0030-assume.c:[[@LINE+3]]:3: error: assumption 'len > 0' is false here [weavec::contradicted-assumption]
  // CHECK: rfc0030-assume.c:[[@LINE-3]]:10: note: 'len' is 0 here
  // LEDGER: "text": "WEAVEC_ASSUME(len>0)",
  WEAVEC_ASSUME(len > 0);
  // LEDGER: "outcome": "violation",
  return len - 1;
}

// CHECK-NOT: error:
// CHECK: 1 error generated.
