// RFC 0032 §3: a guard asks about the address the access uses, whatever expression forms it:
// a pointer sum, a derived local pointer and a pointer walked forwards, each one element outside a
// 16-element heap object, trap at the access.
// STAGE: S3
// RUN-INPUT: 0 16
// RUN-INPUT: 1 16
// RUN-INPUT: 2 16
// RUN-INPUT: 3 16
// RUN-INPUT: 4 16
// RUN-INPUT: 5 16
// RUN-INPUT: 6 16
// RUN-INPUT: 7 16
// ASAN
#include <stdlib.h>
struct pair { int a; long b; };
struct box { char *s, *e; struct pair *v, *ve; struct pair **pp; };
static int plus(struct box *b, long k) {
  return *(b->s + k); // BUG: out-of-bounds // TRAP
}
static int plus_arrow(struct box *b, long k) {
  return (int)(b->v + k)->b; // BUG: out-of-bounds // TRAP
}
static int plus_swapped(struct box *b, long k) {
  return *(k + b->s); // BUG: out-of-bounds // TRAP
}
static int deref_member(struct box *b, long k) {
  return (int)(*(b->v + k)).b; // BUG: out-of-bounds // TRAP
}
static int local(struct box *b, long k) {
  const char *p = b->s + k;
  return *p; // BUG: out-of-bounds // TRAP
}
static int advance(struct box *b, long k) {
  const char *p = b->s;
  p += k;
  return *p; // BUG: out-of-bounds // TRAP
}
static int walk(struct box *b, long k) {
  const char *p = b->s;
  long j;
  for (j = 0; j < k; j++) p++;
  return *p; // BUG: out-of-bounds // TRAP
}
static int postincrement(struct box *b, long k) {
  const char *p = b->s + k;
  return *p++; // BUG: out-of-bounds // TRAP
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
