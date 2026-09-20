// RFC 0030 §3.1, *Aliases of a released object*: a release through an alias a
// join left without the test that made it.
// STAGE: S8
// `put` releases `v`, and on that path `b == v`, so `b` is released too — but
// only there. The `||` joins that path with one where `b` is null, which keeps
// the exact alias edge in the may-relation and drops the test from the path
// guard. The consume of `b` must be recorded under the identity it rests on,
// so the summary says `b: freed when[b == v]`, and `main`, which passes two
// distinct fresh allocations, refutes it instead of reading `release(b)` as a
// second release.
// CLEAN
#include <stdlib.h>

struct box {
  void *slot;
};

static void release(void *p) { free(p); }

static int put(struct box *b, void *v) {
  if (!v)
    return -1;
  if (!b || (void *)b == v) {
    release(v);
    return -1;
  }
  b->slot = v;
  return 0;
}

int main(void) {
  struct box *b = malloc(sizeof *b);
  if (!b)
    return 1;
  if (put(b, malloc(16))) {
    release(b);
    return 1;
  }
  release(b->slot);
  release(b);
  return 0;
}
