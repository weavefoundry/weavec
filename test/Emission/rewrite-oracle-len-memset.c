// RFC 0030, section 10.6, gate G8: memset's byte count against the destination array.
// The -O0 IR equals that of Inputs/rewrite-oracle-len-memset.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-len-memset.expected.c %t

void *memset(void *, int, unsigned long);
void clear(unsigned long n) {
  char b[32];
  memset(b, 0, n);
  memset(b, 1, sizeof b);
}
