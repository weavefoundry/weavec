// RFC 0030, section 10.6, gate G8: a subscript of an array with a constant bound (WrapIndex).
// The -O0 IR equals that of Inputs/rewrite-oracle-index-array.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-index-array.expected.c %t

int get(int i) {
  int a[10] = {0};
  return a[i];
}
void put(int i, int v) {
  int a[10] = {0};
  a[i] = v;
}
