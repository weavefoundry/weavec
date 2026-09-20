// RFC 0030 §13.2 step 5: an inferred requirement (§7.5) a unit exports is
// decided at the callers in other units. `fill` requires `counted(n)` of
// `p` when `0 < n`; `main` passes a whole array of 3 ints and 3, which
// meets it, so A1 counts the requirement verified and the body's
// `trusted(caller-contract)` row becomes proven. `weavec --whole-program`
// and the `weavec-cc` link decide it the same way.
//
// RUN: rm -rf %t && mkdir -p %t
// RUN: %weavec --whole-program --ledger=%t/met.json %s %S/Inputs/rfc0030-fill.c -- 2>&1 | FileCheck --check-prefix=MET %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/met.json
// RUN: %weavec_cc -c %s -o %t/main.o 2>&1 | count 0
// RUN: %weavec_cc -c %S/Inputs/rfc0030-fill.c -o %t/fill.o 2>&1 | count 0
// RUN: %weavec_cc -fweavec-ledger=%t/cc.json %t/main.o %t/fill.o -o %t/prog 2>&1 | FileCheck --check-prefix=CC %s
// RUN: FileCheck --check-prefix=LEDGER %s < %t/cc.json
//
// A call that cannot give the extent leaves the requirement unverified, and
// the Call site carries the reason.
// RUN: %weavec --whole-program --ledger=%t/open.json %S/Inputs/rfc0030-relay.c %S/Inputs/rfc0030-fill.c -- 2>&1 | FileCheck --check-prefix=OPEN %s
// RUN: FileCheck --check-prefix=UNKNOWN %s < %t/open.json

int fill(int *p, int n);

int main(void) {
  int a[3] = {1, 2, 3};
  return fill(a, 3) - 6;
}

// MET: unverified: 0 exported requirements (A1), 0 header invariants (A3)
// CC: unverified: 0 exported requirements (A1), 0 header invariants (A3)
// OPEN: unverified: 1 exported requirement (A1), 0 header invariants (A3)

// The caller's Call row says what the callee needed and what the call gave.
// LEDGER: "A1": {
// LEDGER-NEXT: "exportedRequirements": 1,
// LEDGER-NEXT: "verified": 1,
// LEDGER: "text": "fill(a,3)",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "proven",
// LEDGER: "requirements": [
// LEDGER-NEXT: {
// LEDGER-NEXT: "arg": 0,
// LEDGER-NEXT: "need": "12 bytes",
// LEDGER-NEXT: "have": "12 bytes",
// LEDGER-NEXT: "outcome": "proven"
// The body's row is discharged: every caller in the program meets it.
// LEDGER: "text": "p[i]",
// LEDGER: "spatial": {
// LEDGER-NEXT: "outcome": "proven",
// LEDGER-NEXT: "detail": "every caller of 'fill' in the program meets the requirement",

// UNKNOWN: "exportedRequirements": 1,
// UNKNOWN-NEXT: "verified": 0,
// UNKNOWN: "text": "fill(q,m)",
// UNKNOWN: "spatial": {
// UNKNOWN-NEXT: "outcome": "unresolved",
// UNKNOWN-NEXT: "reason": "unknown-extent",
// UNKNOWN-NEXT: "detail": "'fill' requires counted(param 1 scale 1 plus 0) nonnull of 'p' from its callers, which is not known here",
