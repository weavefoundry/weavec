// RFC 0030, section 10.6, gate G8: compound assignment through a checked pointer.
// Each context uses its own parameter: after a checked dereference the pointer
// is non-null downstream (section 3.2), so a second one would be proven.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-compound-assign.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-compound-assign.expected.c %t

struct counter { int n; };
void bump(int *p, struct counter *s, int *q) {
  *p += 2;
  s->n *= 3;
  q[0] -= 1;
}
