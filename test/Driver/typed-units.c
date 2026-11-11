// RFC 0042 §1, §6, §10: -fweavec-typed. A typed unit calls a function that
// another typed unit defines, and that its declaration marks WEAVEC_SAFE,
// at its body; a declaration that says so of a function defined in a unit
// that is not typed fails to link (T-A2). The ledger lists each function's
// tier, the summary line counts the typed ones, and the audit lists what
// they trust. C++ has no typed units.
// RUN: rm -rf %t && mkdir -p %t/ledger
// RUN: %weavec_cc -O2 -fweavec-typed -c %S/Inputs/typed-callee.c -o %t/callee.o
// RUN: %weavec_cc -O2 -fweavec-typed -fweavec-ledger=%t/ledger/ -c %s -o %t/caller.o 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: %weavec_cc %t/caller.o %t/callee.o -o %t/program
// RUN: %t/program
// RUN: %weavec_cc -O2 -fweavec-safe -c %S/Inputs/typed-callee.c -o %t/safe.o
// RUN: not %weavec_cc %t/caller.o %t/safe.o -o %t/lying 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: %weavec_cc -O2 -fweavec-typed -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: FileCheck %s --check-prefix=LEDGER < %t/ledger/caller.o.ledger.json
// RUN: %weavec_cc --weavec-audit %t/ledger | FileCheck %s --check-prefix=AUDIT
// RUN: %weavec_cc --weavec-audit --format=json %t/ledger | FileCheck %s --check-prefix=JSON
// RUN: not %weavec_cxx -fsyntax-only -fweavec-typed -x c++ %s 2>&1 | FileCheck %s --check-prefix=CXX
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t cap; };

WEAVEC_SAFE int sum(const struct vec *v);

// SUMMARY: weavec: {{.*}}typed-units.c: {{[0-9]+}} accesses: {{.*}}; 1 of 2 functions typed
// IR: call {{.*}}@sum.weavec.safe(
int total(const struct vec *v) { return sum(v); }

// LINK: sum.weavec.safe

// LEDGER: "functions": [
// LEDGER: "function": "total"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "tier": "typed"
// LEDGER: "function": "main"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "tier": "guarded"

// AUDIT: 1 of 2 functions typed
// AUDIT: functions that are not typed (typed code trusts them):
// AUDIT-NEXT: typed-units.c:{{[0-9]+}}: main (guarded)
// JSON: "schema": "weavec-audit"
// JSON: "typed": 1

// CXX: error: safe functions are C only (RFC 0041 §12) [weavec-safe:cxx]
WEAVEC_GUARDED int main(void) {
  struct vec v = {calloc(4, sizeof(int)), 4};
  v.data[3] = 7;
  return total(&v) == 7 ? 0 : 1;
}
