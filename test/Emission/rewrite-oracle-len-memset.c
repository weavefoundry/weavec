// RFC 0030, section 10.6, gate G8: memset's byte count against the destination array.
// The count is in bytes whatever the element type: the `int` array's extent is
// its 32 bytes. Both counts are parameters, since a count the analysis proves,
// such as `sizeof b`, gets no check.
// The -O0 IR equals that of Inputs/rewrite-oracle-len-memset.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-len-memset.expected.c %t

void *memset(void *, int, unsigned long);
void clear(unsigned long n, unsigned long m) {
  char b[32];
  int w[8];
  memset(b, 0, n);
  memset(w, 1, m);
}
