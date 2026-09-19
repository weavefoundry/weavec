// RFC 0030, section 10.6, gate G8: checks in initialisers, conditions, loops and returns.
// Each context uses its own parameter: after a checked dereference the pointer
// is non-null downstream (section 3.2), so a second dereference of it, such as
// a loop body's `*p` after the condition's, would be proven.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-contexts.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-contexts.expected.c %t

struct pair { int a, b; };
struct pair make(int *p, int *q) {
  struct pair r = {*p, q[1]};
  return r;
}
int sum(int *p, int *q, int *r, int *t, int n) {
  int s = 0;
  for (int i = 0; i < n && *p; i++)
    s += *q;
  if (*r > 3)
    return s;
  return -*t;
}
