// RFC 0030 §10.3: the correct twin of span-difference_bug.c: the first and the last element.
// CLEAN
// RUN-INPUT: 0 -15
// RUN-INPUT: 0 0
// RUN-INPUT: 1 16
// RUN-INPUT: 1 1
// ASAN
#include <stdlib.h>
struct box { char *s; long n; };
static int last(struct box *b) { char *e = &b->s[b->n]; return e[-1]; }
static int back(struct box *b, long k) {
  return *(b->s - k);
}
static int before(struct box *b, long i) {
  return *(b->s + i - 1);
}
int main(int argc, char **argv) {
  struct box b;
  long k;
  if (argc < 3) return 2;
  k = atol(argv[2]);
  b.n = 16;
  b.s = calloc(16, 1);
  if (!b.s) return 2;
  return (atol(argv[1]) == 0 ? back(&b, k) : before(&b, k)) + last(&b);
}
