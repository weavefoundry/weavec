// RFC 0030, section 10.6, gate G8: len checks sequenced before the call with the comma operator, and disjoint on the destination.
// Source and destination lie in one array, so they may overlap (the overlap
// check of two distinct arrays is proven); the source's extent is what remains
// of the array after its offset of 8.
// The -O0 IR equals that of Inputs/rewrite-oracle-len-before-call.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-len-before-call.expected.c %t

void *memcpy(void *, const void *, unsigned long);
void copy(unsigned long n) {
  char b[32] = {0};
  char *s = b + 8;
  memcpy(b, s, n);
}
