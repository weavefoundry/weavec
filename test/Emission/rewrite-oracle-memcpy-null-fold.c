// RFC 0030, section 10.6, gate G8: memcpy(NULL, x, 0) passes, and the later dereference is still checked (§8.3).
// The -O0 IR equals that of Inputs/rewrite-oracle-memcpy-null-fold.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-memcpy-null-fold.expected.c %t

void *memcpy(void *, const void *, unsigned long);
int fold(int *p, const int *x) {
  memcpy(p, x, 0);
  return *p;
}
