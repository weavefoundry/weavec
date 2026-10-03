// RFC 0032 §3: the correct twin of access-subscript_bug.c: each shape at the first and the last
// element it may use passes its guard.
// CLEAN
// STAGE: S3
// RUN-INPUT: 0 0
// RUN-INPUT: 0 15
// RUN-INPUT: 1 0
// RUN-INPUT: 1 15
// RUN-INPUT: 2 0
// RUN-INPUT: 2 15
// RUN-INPUT: 3 1
// RUN-INPUT: 3 15
// RUN-INPUT: 4 0
// RUN-INPUT: 4 15
// RUN-INPUT: 5 0
// RUN-INPUT: 5 15
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct box { char *s, *e; struct pair *v, *ve; struct pair **pp; };
static int subscript_member(struct box *b, long k) {
  return (int)b->v[k].b;
}
static int address_arrow(struct box *b, long k) {
  return (&b->v[k])->a;
}
static int two_levels(struct box *b, long k) {
  return (int)b->pp[0][k].b;
}
static int copy(struct box *b, long k) {
  b->v[k] = b->v[0];
  return 0;
}
static int compound(struct box *b, long k) {
  b->s[k] += 1;
  return 0;
}
static int loop_write(struct box *b, long k) {
  long j;
  for (j = 0; j <= k; j++) b->s[j] = 0;
  return 0;
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
  case 0: return subscript_member(&b, k) != 0;
  case 1: return address_arrow(&b, k) != 0;
  case 2: return two_levels(&b, k) != 0;
  case 3: return copy(&b, k) != 0;
  case 4: return compound(&b, k) != 0;
  case 5: return loop_write(&b, k) != 0;
  default: return 2;
  }
}
