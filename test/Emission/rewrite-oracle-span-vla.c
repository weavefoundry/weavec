// RFC 0030, section 10.6, gate G8: the span index form: a variable-length array element is addressed only after its check.
// The -O0 IR equals that of Inputs/rewrite-oracle-span-vla.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-span-vla.expected.c %t

int get(int n, int i) {
  int v[n];
  v[0] = 1;
  return v[i];
}
