// RFC 0033 §7: what the link step does. By default (records) it checks the
// records against each other and composes the program ledger from their
// rows, with each facet's check and requirements, without analysing any
// unit again: a use after a release in another unit is not reported at the
// link (its guard traps when it happens). -fweavec-link=analyze analyses
// the units again and reports it; past its budget a unit keeps its
// compile-time results, with a note, and the link goes ahead.
// -fweavec-link=none links as Clang.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o
// RUN: %weavec_cc -c %S/Inputs/link-modes-release.c -o %t/release.o
// RUN: %weavec_cc -fweavec-ledger=%t/records.json %t/main.o %t/release.o -o %t/records 2>&1 | FileCheck --check-prefix=RECORDS %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/records.json
// RUN: not --crash %t/records
// RUN: not %weavec_cc -fweavec-link=analyze %t/main.o %t/release.o -o %t/analyzed 2>&1 | FileCheck --check-prefix=ANALYZE %s
// RUN: %weavec_cc -fweavec-link=analyze -fweavec-link-budget=0.000001 %t/main.o %t/release.o -o %t/cut 2>&1 | FileCheck --check-prefix=CUT %s
// RUN: %weavec_cc -fweavec-link=none %t/main.o %t/release.o -o %t/none 2>&1 | count 0
// RUN: not %weavec_cc -fno-weavec-link %t/main.o %t/release.o -o %t/old 2>&1 | FileCheck --check-prefix=OLD %s
// RUN: not %weavec_cc -fweavec-link=fast %t/main.o %t/release.o -o %t/bad 2>&1 | FileCheck --check-prefix=BAD %s

#include <stdlib.h>

void release(int *p);

int main(void) {
  int *p = malloc(sizeof *p);
  if (p == NULL)
    return 1;
  *p = 1;
  release(p);
  return *p;
}

// RECORDS-NOT: error
// RECORDS: weavec: program records: {{.*}}0 errors
// The rows come from the records, checks and all.
// LEDGER: "scope": "program"
// LEDGER: "outcome": "guarded",
// LEDGER-NEXT: "reason": "unknown-callee",
// LEDGER: "check": {
// LEDGER-NEXT: "template": "live"
// ANALYZE: error: use of 'p' after it was freed [weavec::use-after-free]
// CUT: weavec-cc: note: the whole-program analysis of '{{.*}}' stopped at its budget; their compile-time results stand
// OLD: unknown WeaveC flag '-fno-weavec-link'
// BAD: invalid value 'fast' in '-fweavec-link=fast'
