// RFC 0032 §3: the correct twin of access-sum_bug.c: each shape at the first and the last
// element it may use passes its guard.
// CLEAN
// STAGE: S3
// RUN-INPUT: 0 0
// RUN-INPUT: 0 15
// RUN-INPUT: 1 0
// RUN-INPUT: 1 15
// RUN-INPUT: 2 0
// RUN-INPUT: 2 15
// RUN-INPUT: 3 0
// RUN-INPUT: 3 15
// RUN-INPUT: 4 0
// RUN-INPUT: 4 15
// RUN-INPUT: 5 0
// RUN-INPUT: 5 15
// RUN-INPUT: 6 0
// RUN-INPUT: 6 15
// RUN-INPUT: 7 0
// RUN-INPUT: 7 15
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct box { char *s, *e; struct pair *v, *ve; struct pair **pp; };
static int plus(struct box *b, long k) {
  return *(b->s + k);
}
static int plus_arrow(struct box *b, long k) {
  return (int)(b->v + k)->b;
}
static int plus_swapped(struct box *b, long k) {
  return *(k + b->s);
}
static int deref_member(struct box *b, long k) {
  return (int)(*(b->v + k)).b;
}
static int local(struct box *b, long k) {
  const char *p = b->s + k;
  return *p;
}
static int advance(struct box *b, long k) {
  const char *p = b->s;
  p += k;
  return *p;
}
static int walk(struct box *b, long k) {
  const char *p = b->s;
  long j;
  for (j = 0; j < k; j++) p++;
  return *p;
}
static int postincrement(struct box *b, long k) {
  const char *p = b->s + k;
  return *p++;
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
  case 0: return plus(&b, k) != 0;
  case 1: return plus_arrow(&b, k) != 0;
  case 2: return plus_swapped(&b, k) != 0;
  case 3: return deref_member(&b, k) != 0;
  case 4: return local(&b, k) != 0;
  case 5: return advance(&b, k) != 0;
  case 6: return walk(&b, k) != 0;
  case 7: return postincrement(&b, k) != 0;
  default: return 2;
  }
}
