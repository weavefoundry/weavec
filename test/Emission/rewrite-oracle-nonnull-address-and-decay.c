// RFC 0030, section 10.6, gate G8: `&p->f` and a decaying array member keep the null check only.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-address-and-decay.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-address-and-decay.expected.c %t

struct record { int n; char name[8]; };
int *field(struct record *r) { return &r->n; }
char *name(struct record *r) { return r->name; }
