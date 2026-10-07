// RFC 0032 §3: a guard asks about the address the access uses, whatever expression forms it:
// a pointer difference, a negative subscript and a pointer walked backwards, each one element outside a
// 16-element heap object, trap at the access.
// STAGE: S3
// RUN-INPUT: 0 17
// RUN-INPUT: 1 17
// RUN-INPUT: 2 0
// RUN-INPUT: 3 17
// RUN-INPUT: 4 17
// RUN-INPUT: 5 16
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct box { char *s, *e; struct pair *v, *ve; struct pair **pp; };
static int minus(struct box *b, long k) {
  return *(b->e - k); // BUG: out-of-bounds // TRAP
}
static int minus_arrow(struct box *b, long k) {
  return (int)(b->ve - k)->b; // BUG: out-of-bounds // TRAP
}
static int minus_end(struct box *b, long k) {
  return *(b->e - k); // BUG: out-of-bounds // TRAP
}
static int negative(struct box *b, long k) {
  return b->e[-k]; // BUG: out-of-bounds // TRAP
}
static int walk_back(struct box *b, long k) {
  const char *p = b->e;
  long j;
  for (j = 0; j < k; j++) --p;
  return *p; // BUG: out-of-bounds // TRAP
}
static int predecrement(struct box *b, long k) {
  const char *p = b->e - k;
  return *--p; // BUG: out-of-bounds // TRAP
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
