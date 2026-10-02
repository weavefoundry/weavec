// RFC 0032 §3: the correct twin of access-difference_bug.c: each shape at the first and the last
// element it may use passes its guard.
// CLEAN
// STAGE: S3
// RUN-INPUT: 0 1
// RUN-INPUT: 0 16
// RUN-INPUT: 1 1
// RUN-INPUT: 1 16
// RUN-INPUT: 2 1
// RUN-INPUT: 3 1
// RUN-INPUT: 3 16
// RUN-INPUT: 4 1
// RUN-INPUT: 4 16
// RUN-INPUT: 5 0
// RUN-INPUT: 5 15
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct box { char *s, *e; struct pair *v, *ve; struct pair **pp; };
static int minus(struct box *b, long k) {
  return *(b->e - k);
}
static int minus_arrow(struct box *b, long k) {
  return (int)(b->ve - k)->b;
}
static int minus_end(struct box *b, long k) {
  return *(b->e - k);
}
static int negative(struct box *b, long k) {
  return b->e[-k];
}
static int walk_back(struct box *b, long k) {
  const char *p = b->e;
  long j;
  for (j = 0; j < k; j++) --p;
  return *p;
}
static int predecrement(struct box *b, long k) {
  const char *p = b->e - k;
  return *--p;
}
int main(int argc, char **argv) {
  struct box b;
  long shape, k;
  if (argc < 3) return 2;
  shape = atol(argv[1]);
  k = atol(argv[2]);
  b.s = calloc(16, 1);
  b.v = calloc(16, sizeof(struct pair));
  b.pp = malloc(sizeof *b.pp);
  if (!b.s || !b.v || !b.pp) return 2;
  b.e = b.s + 16;
  b.ve = b.v + 16;
  b.pp[0] = b.v;
  switch (shape) {
  case 0: return minus(&b, k) != 0;
  case 1: return minus_arrow(&b, k) != 0;
  case 2: return minus_end(&b, k) != 0;
  case 3: return negative(&b, k) != 0;
  case 4: return walk_back(&b, k) != 0;
  case 5: return predecrement(&b, k) != 0;
  default: return 2;
  }
}
