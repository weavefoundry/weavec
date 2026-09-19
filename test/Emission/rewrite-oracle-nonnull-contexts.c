// RFC 0030, section 10.6, gate G8: checks in initialisers, conditions, loops and returns.
// The -O0 IR equals that of Inputs/rewrite-oracle-nonnull-contexts.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-nonnull-contexts.expected.c %t

struct pair { int a, b; };
struct pair make(int *p) {
  struct pair r = {*p, p[1]};
  return r;
}
int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n && *p; i++)
    s += *p;
  if (*p > 3)
    return s;
  return -*p;
}
