// RFC 0030, section 10.6, gate G8: a whole struct read and written through checked pointers.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-struct-value.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-struct-value.expected.c %t

struct big { int v[4]; };
struct big get(struct big *p) { return *p; }
void put(struct big *p, struct big b) { *p = b; }
