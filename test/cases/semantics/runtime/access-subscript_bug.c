// RFC 0032 §3: a guard asks about the address the access uses, whatever expression forms it:
// a subscript with a member, through two levels, as a store, and in a loop, each one element outside a
// 16-element heap object, trap at the access.
// STAGE: S3
// RUN-INPUT: 0 16
// RUN-INPUT: 1 16
// RUN-INPUT: 2 16
// RUN-INPUT: 3 16
// RUN-INPUT: 4 16
// RUN-INPUT: 5 16
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct box { char *s, *e; struct pair *v, *ve; struct pair **pp; };
static int subscript_member(struct box *b, long k) {
  return (int)b->v[k].b; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
static int address_arrow(struct box *b, long k) {
  return (&b->v[k])->a; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
static int two_levels(struct box *b, long k) {
  return (int)b->pp[0][k].b; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
static int copy(struct box *b, long k) {
  b->v[k] = b->v[0]; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
  return 0;
}
static int compound(struct box *b, long k) {
  b->s[k] += 1; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
  return 0;
}
static int loop_write(struct box *b, long k) {
  long j;
  for (j = 0; j <= k; j++) b->s[j] = 0; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
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
