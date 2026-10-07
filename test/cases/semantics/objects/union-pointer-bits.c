// RFC 0031 §4.2 (unions), RFC 0030 §2.3: a pointer member loaded from a cell whose last store was of a non-pointer type is unknown with reason raw-cast; one whose last store was a pointer keeps its value.
// STAGE: S2
// 'u.bits' is written with the bits of '&x' and read back as 'u.p': a reinterpretation, so
// the dereference's spatial facet is unresolved(raw-cast), never proven (the program is
// correct, so it runs clean). 'w.p' is written as a pointer and read as one: its value, and
// the proof of its dereference, are kept.
// CLEAN
// ASAN
#include <stdint.h>
union pun { int *p; uintptr_t bits; };
int main(void) {
  int x = 5, y = 6;
  union pun u, w;
  u.bits = (uintptr_t)&x;
  int r = *u.p;
  w.p = &y;
  r += *w.p;
  return r == 11 ? 0 : 1;
}
