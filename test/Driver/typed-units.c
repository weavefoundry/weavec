// RFC 0042 §1, §6, §10; RFC 0043 §1, §11: -fweavec-safe. A safe unit calls
// a function that another safe unit defines, and that its declaration marks
// WEAVEC_SAFE, at its body; a declaration that says so of a function
// defined in a unit that is not safe fails to link (T-A2). The ledger lists
// each function's tier and each pointer field's kind, the summary line
// counts them, and the audit lists what safe code trusts. -fweavec-typed is
// gone. (Safe C++: safe-cxx-units.cpp.)
// RUN: rm -rf %t && mkdir -p %t/ledger
// RUN: %weavec_cc -O2 -fweavec-safe -c %S/Inputs/typed-callee.c -o %t/callee.o
// RUN: %weavec_cc -O2 -fweavec-safe -fweavec-ledger=%t/ledger/ -c %s -o %t/caller.o 2>&1 | FileCheck %s --check-prefix=SUMMARY
// RUN: %weavec_cc %t/caller.o %t/callee.o -o %t/program
// RUN: %t/program
// RUN: %weavec_cc -O2 -c %S/Inputs/typed-callee.c -o %t/guarded.o
// RUN: not %weavec_cc %t/caller.o %t/guarded.o -o %t/lying 2>&1 | FileCheck %s --check-prefix=LINK
// RUN: %weavec_cc -O2 -fweavec-safe -S -emit-llvm %s -o - | FileCheck %s --check-prefix=IR
// RUN: FileCheck %s --check-prefix=LEDGER < %t/ledger/caller.o.ledger.json
// RUN: %weavec_cc --weavec-audit %t/ledger | FileCheck %s --check-prefix=AUDIT
// RUN: %weavec_cc --weavec-audit --format=json %t/ledger | FileCheck %s --check-prefix=JSON
// RUN: mkdir -p %t/twice && cp %t/ledger/caller.o.ledger.json %t/twice/a.o.ledger.json && cp %t/ledger/caller.o.ledger.json %t/twice/b.o.ledger.json
// RUN: %weavec_cc --weavec-audit %t/twice | FileCheck %s --check-prefix=TWICE
// RUN: not %weavec_cc -fsyntax-only -fweavec-typed %s 2>&1 | FileCheck %s --check-prefix=TYPED
#include <weavec.h>
#include <stdlib.h>

struct vec { int *WEAVEC_OWNED WEAVEC_COUNTED_BY(cap) data; size_t cap; };
struct view { struct vec *of; };

WEAVEC_SAFE int sum(const struct vec *v);

// SUMMARY: weavec: {{.*}}typed-units.c: {{[0-9]+}} accesses: {{.*}}; 2 of 3 functions safe; 1 owner and 1 reference fields
// IR: call {{.*}}@sum.weavec.safe(
int total(const struct vec *v) { return sum(v); }

int viewed(const struct view *w) { return total(w->of); }

// LINK: sum.weavec.safe

// LEDGER: "fields": [
// LEDGER: "field": "data"
// LEDGER: "kind": "owner"
// LEDGER: "field": "of"
// LEDGER: "kind": "reference"
// LEDGER: "functions": [
// LEDGER: "function": "total"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "owns": false
// LEDGER-NEXT: "tier": "safe"
// LEDGER: "function": "main"
// LEDGER-NEXT: "line"
// LEDGER-NEXT: "owns": false
// LEDGER-NEXT: "tier": "guarded"
// LEDGER: "reason": "reference"
// LEDGER: "version": 7

// AUDIT: 2 of 3 functions safe
// AUDIT: functions that are not safe (safe code trusts them
// AUDIT-NEXT: typed-units.c:{{[0-9]+}}: main (guarded)
// AUDIT: reference fields (tested where safe code loads them):
// AUDIT-NEXT: typed-units.c:{{[0-9]+}}: view.of
// A header's struct, in two units' ledgers, is listed once.
// TWICE: reference fields
// TWICE-NEXT: view.of
// TWICE-NOT: view.of
// JSON-DAG: "schema": "weavec-audit"
// JSON-DAG: "safe": 2
// JSON-DAG: "version": 3

// TYPED: '-fweavec-typed' was removed: -fweavec-safe makes a unit safe, with typed memory and ownership (RFC 0043)
WEAVEC_GUARDED int main(void) {
  struct vec v = {calloc(4, sizeof(int)), 4};
  struct view w = {&v};
  v.data[3] = 7;
  int r = viewed(&w) == 7 ? 0 : 1;
  free(v.data);
  return r;
}
