// RFC 0030, section 10.6, gate G8: an array member through a checked pointer.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-member-array.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-member-array.expected.c %t

struct buf { char data[16]; int len; };
char at(struct buf *b, int i) { return b->data[i]; }
