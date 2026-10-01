// RFC 0030, section 10.6, gate G8: the span index form: a variable-length array element is addressed only after its check.
// The -O0 IR equals that of Inputs/rewrite-oracle-span-vla.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-span-vla.expected.c %t
// RUN: %weavec --ledger=%t.json %s --
// RUN: FileCheck %s < %t.json

// RFC 0031 *Implementation amendments*, "Variable-length arrays": `n` may be
// zero or negative, which gives `v` no storage to prove or check an access
// in. Both accesses are unresolved(unknown-extent), never proven, and get no
// check; the old engine checked both against `sizeof(v)`
// (test/cases/KNOWN-DIFFERENCES.md, *Lit tests*).
// CHECK: "name": "get",
// CHECK: "text": "v[0]",
// CHECK: "spatial": {
// CHECK-NEXT: "outcome": "unresolved",
// CHECK-NEXT: "reason": "unknown-extent",
// CHECK: "text": "v[i]",
// CHECK: "spatial": {
// CHECK-NEXT: "outcome": "unresolved",
// CHECK-NEXT: "reason": "unknown-extent",
int get(int n, int i) {
  int v[n];
  v[0] = 1;
  return v[i];
}

// With a positive dimension the storage is `n` elements: `v[0]` is proven,
// and `v[i]` is checked in the span form.
// CHECK: "name": "get_positive",
// CHECK: "text": "v[0]",
// CHECK: "spatial": {
// CHECK-NEXT: "outcome": "proven",
// CHECK: "text": "v[i]",
// CHECK: "spatial": {
// CHECK-NEXT: "outcome": "checked",
int get_positive(int n, int i) {
  if (n < 1)
    return 0;
  int v[n];
  v[0] = 1;
  return v[i];
}
