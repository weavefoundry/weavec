// RFC 0030, section 10.6, gate G8: bit-field members through a checked pointer.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-bitfield.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-bitfield.expected.c %t

struct flags { unsigned a : 3; unsigned b : 5; };
unsigned getb(struct flags *f) { return f->b; }
void setb(struct flags *f) { f->b = 7; }
