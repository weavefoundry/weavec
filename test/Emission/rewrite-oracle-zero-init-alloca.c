// RFC 0030, section 10.6, gate G8: alloca(n) is zeroed at the site: __builtin_memset(alloca(n), 0, n).
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-alloca.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-alloca.expected.c %t

int stack(unsigned long n) {
  char *p = __builtin_alloca(n);
  return p[0];
}
