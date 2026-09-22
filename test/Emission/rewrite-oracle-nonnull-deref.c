// RFC 0030, section 10.6, gate G8: a dereference's pointer is checked in place, for a load and a store.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-deref.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-deref.expected.c %t

int load(int *p) { return *p; }
void store(int *p, int v) { *p = v; }
