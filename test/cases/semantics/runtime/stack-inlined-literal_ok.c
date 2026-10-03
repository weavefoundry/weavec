// RFC 0032 §4.3, amendment 21: a function with automatic storage the object list does not know
// (a compound literal) says so even when it has nothing to enter, because once it is inlined
// that storage lies in its caller's frame: here right after 'a', where a pointer to it would
// otherwise be taken for one past 'a'.
// STAGE: S8
// CLEAN
// FLAGS: -O2
// RUN-INPUT:
#include <stdio.h>
static __attribute__((noinline)) long rd(const void *p) { return *(const long *)p; }
static __attribute__((noinline)) void wr(void *p, long v) { *(long *)p = v; }
static long g(long v) { return rd((long[1]){v}); }
static __attribute__((noinline)) long f(long v) {
  long a, b, c, d;
  wr(&a, v);
  wr(&b, v);
  wr(&c, v);
  wr(&d, v);
  return g(v) + rd(&a) + rd(&b) + rd(&c) + rd(&d);
}
int main(int argc, char **argv) {
  (void)argv;
  return f(argc) != 5 * argc;
}
