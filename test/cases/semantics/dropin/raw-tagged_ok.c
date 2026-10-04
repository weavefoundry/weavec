// RFC 0033 §2: a tagged pointer (low bits used as a tag) round-trips through an integer; its
// uses are guarded, not errors, and the guards pass on the real object.
// STAGE: S1
// CLEAN
// RUN-INPUT:
#include <stdint.h>
#include <stdlib.h>
struct node { int payload; };
static uintptr_t tag(struct node *n, unsigned t) { return (uintptr_t)n | (t & 7u); }
static struct node *untag(uintptr_t tp) { return (struct node *)(tp & ~(uintptr_t)7); }
static unsigned low_bits(void *p) { return (unsigned)((uintptr_t)p & 7); }
static int same(void *a, void *b) { return a == b; }
int main(void) {
  struct node *n = malloc(sizeof *n);
  if (n == NULL) return 1;
  n->payload = 5;
  uintptr_t tp = tag(n, 3);
  void *raw = (void *)tp;
  int r = untag(tp)->payload + (int)low_bits(raw) + same(raw, raw);
  free(untag(tp));
  return r == 9 ? 0 : 1;
}
