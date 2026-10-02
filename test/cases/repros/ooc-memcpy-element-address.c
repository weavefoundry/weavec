// Held-out repro (RFC 0031 Motivation, §11.1): lz4 lib/lz4.c:561 (LZ4_memcpy_using_offset,
// offset 2) copies the first half of an 8-byte stack buffer into its second half with
// 'LZ4_memcpy(&v[4], v, 4)'. The ranges [4, 8) and [0, 4) are disjoint.
// v0.11.0 reports "'memcpy' copies 4 bytes between overlapping ranges of 'v'" (a false
// definite out-of-bounds error, and a 'violation' trap when lowered), while the same copy
// spelled 'memcpy(v + 4, v, 4)' is accepted. Reduced from build/rfc31/ooc/repro/ov.c.
// intended: no finding; the disjoint facet of '&v[k]' is judged like 'v + k'.
// CLEAN
// ASAN
#include <string.h>
void f(unsigned char *d, const unsigned char *s) {
  unsigned char v[8];
  memcpy(v, s, 2);
  memcpy(&v[2], s, 2);
  memcpy(&v[4], v, 4);
  memcpy(d, v, 8);
}
void g(unsigned char *d) {
  unsigned char v[8] = {0};
  __builtin_memcpy(&v[4], v, 4);
  memcpy(d, v, 8);
}
void h(unsigned char *d) {
  unsigned char v[8] = {0};
  memcpy(v + 4, v, 4);
  memcpy(d, v, 8);
}
void k(unsigned char *d) {
  unsigned char v[8] = {0};
  memcpy(v, v + 4, 4);
  memcpy(d, v, 8);
}
int main(void) {
  unsigned char src[2] = {1, 2}, out[8];
  f(out, src);
  if (out[6] != 1 || out[7] != 2) return 1;
  g(out);
  h(out);
  k(out);
  return out[0];
}
