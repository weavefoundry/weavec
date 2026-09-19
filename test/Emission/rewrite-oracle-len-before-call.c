// RFC 0030, section 10.6, gate G8: len checks sequenced before the call with the comma operator, and disjoint on the destination.
// The -O0 IR equals that of Inputs/rewrite-oracle-len-before-call.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-len-before-call.expected.c %t

void *memcpy(void *, const void *, unsigned long);
void copy(unsigned long n) {
  char d[16];
  char s[16] = {0};
  memcpy(d, s, n);
}
