// RFC 0030, section 10.6, gate G8: a constant subscript is checked too when the analysis cannot decide it.
// Against an array's own bound a constant subscript is proven or a violation,
// so this one indexes an allocation whose size is a parameter: `a[2]` is
// checked against `n`, after the allocation result's null check.
// The -O0 IR equals that of Inputs/rewrite-oracle-index-constant.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-constant.expected.c %t

void *malloc(unsigned long);
char *third(unsigned long n) {
  char *a = malloc(n);
  a[2] = 1;
  return a;
}
