// RFC 0030, section 10.6, gate G8: the result of a call is checked where it is dereferenced.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-call-result.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-call-result.expected.c %t

struct node { int value; struct node *next; };
struct node *next(struct node *n);
int after(struct node *n) { return next(n)->value; }
