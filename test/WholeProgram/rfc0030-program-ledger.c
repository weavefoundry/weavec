// RFC 0030 §13.2 step 6 and §12.4: `weavec --whole-program` and the link of
// `weavec-cc` compose one program ledger (scope `program`) from the units'
// runs and print the program's summary line. `first` relies on its
// parameter's Single default (§7.3) and the caller in the other unit passes
// a cursor, so the Call site gets `unresolved(unknown-extent)` (step 5),
// counted under A1.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec --whole-program --ledger=%t/program.json %s %S/Inputs/rfc0030-first.c -- 2>&1 | FileCheck --check-prefix=SUMMARY %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/program.json
// RUN: %weavec --whole-program --ledger=%t/dir/ %s %S/Inputs/rfc0030-first.c -- 2>&1 | FileCheck --check-prefix=SUMMARY %s
// RUN: ls %t/dir/rfc0030-program-ledger.ledger.json
//
// The same through weavec-cc: the summary line under -fweavec-summary or
// with -fweavec-ledger, and otherwise a link as quiet as Clang's.
// RUN: %weavec_cc -c %s -o %t/main.o 2>&1 | count 0
// RUN: %weavec_cc -c %S/Inputs/rfc0030-first.c -o %t/first.o 2>&1 | count 0
// RUN: %weavec_cc -fweavec-summary %t/main.o %t/first.o -o %t/prog 2>&1 | FileCheck --check-prefix=CC %s
// RUN: %weavec_cc -fweavec-ledger=%t/cc.json %t/main.o %t/first.o -o %t/prog 2>&1 | FileCheck --check-prefix=CC %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/cc.json
// RUN: %weavec_cc %t/main.o %t/first.o -o %t/quiet 2>&1 | count 0
// RUN: %t/quiet

int first(int *p);

int main(void) {
  int a[4] = {1, 2, 3, 4};
  return first(a + 1) - 2;
}

// SUMMARY: weavec: program rfc0030-program-ledger: {{[0-9]+}} sites in 2 units: {{.*}}; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)
// CC: weavec: program prog: {{[0-9]+}} sites in 2 units: {{.*}}; 0 errors, 0 warnings; unverified: 0 exported requirements (A1), 0 header invariants (A3)

// LEDGER: "scope": "program",
// LEDGER: "A1": {
// LEDGER-NEXT: "exportedRequirements": 0,
// LEDGER-NEXT: "verified": 0,
// LEDGER-NEXT: "reliesOnSingle": 1,
// LEDGER-NEXT: "unverifiedCallers": 1
// LEDGER: "units": [
// LEDGER: "source": "{{.*}}rfc0030-program-ledger.c",
// LEDGER: "text": "first(a+1)",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "unresolved",
// LEDGER-NEXT: "reason": "unknown-extent",
// LEDGER-NEXT: "detail": "'first' relies on the argument for 'p' pointing to at least one element, which is not known here",
// LEDGER: "source": "{{.*}}rfc0030-first.c",
