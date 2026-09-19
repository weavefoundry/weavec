// RFC 0030, section 10.6, gate G8: `++` and `--` through a checked pointer.
// Each context uses its own parameter: after a checked dereference the pointer
// is non-null downstream (section 3.2), so a second one would be proven.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-increment.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-increment.expected.c %t

struct counter { int n; };
void step(int *p, struct counter *s, struct counter *t, int *q) {
  (*p)++;
  ++s->n;
  t->n--;
  --*q;
}
