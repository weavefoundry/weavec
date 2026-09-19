// RFC 0030 §5.5: a function whose analysis would take more block transfers
// than its budget stops; its operations take the §2.6 defaults with reason
// `budget`, the summary line and the ledger list it, and its callers apply
// the unknown-callee default at its calls.
// RUN: %weavec --budget=12 --ledger=%t.json %s -- 2>&1 | FileCheck %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t.json
// RUN: %weavec --budget=0 --ledger=%t.full.json %s -- 2>&1 | FileCheck --check-prefix=FULL %s
#include <stdlib.h>

// CHECK: rfc0030-budget.c: {{.*}}; 1 function over budget (walk)
// FULL-NOT: over budget
// LEDGER: "overBudget": [
// LEDGER-NEXT: "walk"
// LEDGER-NEXT: ]
int walk(const int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    for (int j = 0; j < i; j++)
      if (p[i] > p[j])
        // LEDGER: "text": "p[j]",
        // LEDGER: "spatial": {
        // LEDGER-NEXT: "outcome": "unresolved",
        // LEDGER-NEXT: "reason": "budget",
        // LEDGER: "temporal": {
        // LEDGER-NEXT: "outcome": "unresolved",
        // LEDGER-NEXT: "reason": "budget",
        s += p[j];
  return s;
}

// The caller stays within its budget; the call to `walk` may have done
// anything to what it was handed.
int caller(void) {
  int *p = malloc(4 * sizeof *p);
  if (!p)
    return 0;
  p[0] = 1;
  // LEDGER: "text": "walk(p,4)",
  // LEDGER: "outcome": "unresolved",
  // LEDGER-NEXT: "reason": "budget",
  int s = walk(p, 4);
  // LEDGER: "text": "p[0]",
  // LEDGER: "temporal": {
  // LEDGER-NEXT: "outcome": "unresolved",
  // LEDGER-NEXT: "reason": "unknown-callee",
  s += p[0];
  free(p);
  return s;
}
