// RFC 0030 §13.2 step 3 (probe 38): a declaration that says WEAVEC_BORROWED
// while the definition in another unit frees the parameter is an
// `annotation-mismatch` error at the declaration when weavec-cc links the
// objects, and the link fails; `weavec --whole-program` reports the same.
// Step 6: the program ledger, written under -fweavec-ledger with the
// summary line, records the error, and the temporal facets that rested on
// the contradicted declaration prove nothing.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec_cc -c %s -o %t/main.o 2>&1 | count 0
// RUN: %weavec_cc -c %S/Inputs/rfc0030-inspect.c -o %t/inspect.o 2>&1 | count 0
// RUN: not %weavec_cc %t/main.o %t/inspect.o -o %t/prog 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: not ls %t/prog
// RUN: not %weavec --whole-program %s %S/Inputs/rfc0030-inspect.c -- 2>&1 | FileCheck --check-prefix=LINK %s
//
// RUN: not %weavec_cc -fweavec-ledger=%t/ledgers/ %t/main.o %t/inspect.o -o %t/prog 2>&1 | FileCheck --check-prefix=SUMMARY %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/ledgers/prog.ledger.json
//
// Lowered to a warning, the error lets the link go ahead.
// RUN: %weavec_cc -Wno-error=weavec-annotation-mismatch %t/main.o %t/inspect.o -o %t/lowered 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: ls %t/lowered
#include <stdlib.h>
#include <weavec.h>

// LINK: rfc0030-link-declarations.c:[[@LINE+2]]:6: error: 'inspect' is declared WEAVEC_BORROWED here but its definition in '{{.*}}rfc0030-inspect.c' frees 'p' [weavec::annotation-mismatch]
// LINK: rfc0030-inspect.c:4:6: note: defined here
void inspect(char *WEAVEC_BORROWED p);

int main(void) {
  char *p = malloc(8);
  if (!p)
    return 1;
  p[0] = 1;
  inspect(p);
  int r = p[0];
  free(p);
  return r;
}

// SUMMARY: error: 'inspect' is declared WEAVEC_BORROWED here
// SUMMARY: weavec: program prog: {{[0-9]+}} sites in 2 units: {{.*}}; 1 error, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)

// LEDGER: "scope": "program",
// LEDGER: "A1": {
// LEDGER: "units": [
// LEDGER: "source": "{{.*}}rfc0030-link-declarations.c",
// LEDGER: "text": "p[0]",
// LEDGER: "reason": "unknown-callee",
// LEDGER-NEXT: "detail": "the declaration of 'inspect' (WEAVEC_BORROWED) is contradicted by its definition, which frees 'p'"
// LEDGER: "source": "{{.*}}rfc0030-inspect.c",
// LEDGER: "diagnostics": [
// LEDGER: "id": "annotation-mismatch",
// LEDGER-NEXT: "severity": "error",

// LOWERED: warning: 'inspect' is declared WEAVEC_BORROWED here but its definition in '{{.*}}rfc0030-inspect.c' frees 'p' [weavec::annotation-mismatch]
