// RFC 0030, section 10.6, gate G8: -fweavec-checks=none: no check, no prelude, no zero-initialisation.
// The -O0 IR equals that of Inputs/rewrite-oracle-checks-none.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-checks-none.expected.c %t -- -fweavec-checks=none -Wno-error=weavec

void *malloc(unsigned long);
int load(int *p, int i) {
  int a[4];
  int *q = malloc(4);
  a[i] = *p;
  return a[i] + *q;
}
