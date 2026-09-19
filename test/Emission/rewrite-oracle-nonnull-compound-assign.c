// RFC 0030, section 10.6, gate G8: compound assignment through a checked pointer.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-compound-assign.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-compound-assign.expected.c %t

struct counter { int n; };
void bump(int *p, struct counter *s) {
  *p += 2;
  s->n *= 3;
  p[0] -= 1;
}
