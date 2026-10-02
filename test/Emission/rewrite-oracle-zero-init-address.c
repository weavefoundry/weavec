// RFC 0030, section 10.6, gate G8: taking an allocator's address yields a static non-inline wrapper.
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-address.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-address.expected.c %t
// RUN: %weavec --ledger=%t.json %s --
// RUN: FileCheck %s < %t.json
//
// The call through `f` needs no null check: `f` holds the address of
// `malloc`, which the object engine knows (RFC 0031 §4.1), so the facet is
// proven. rewrite-oracle-nonnull-function-pointer.c keeps the checked form.
// CHECK: "text": "f(n)",
// CHECK: "null": {
// CHECK-NEXT: "outcome": "proven",

void *malloc(unsigned long);
void *(*allocate)(unsigned long) = malloc;
void *make(unsigned long n) {
  void *(*f)(unsigned long) = malloc;
  return f(n);
}
