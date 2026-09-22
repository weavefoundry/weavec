// RFC 0030, section 10.6, gate G8: nested accesses: each pointer is checked once, innermost first.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-nested.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-nested.expected.c %t

struct node { int value; struct node *next; };
int second(struct node *n) { return n->next->value; }
int twice(int **pp) { return **pp; }
