// RFC 0009, *Scalar facts in the state*: "Join: intersection of the domains,
// join of the facts". A plain `if`/`else` merge is not a loop coming round
// again, so it must not widen the ranges it merges away.
// STAGE: S8
// RFC 0030 §8.2 makes `realloc(p, 0)` a release of `p`, so `reserve`'s
// `if (grown == NULL) free(*slot);` is a double free exactly when the size can
// be zero. Both arms of the size choice leave `size` at least 1: one assigns a
// constant, the other copies a `want` the guards pin to `[1, INT_MAX/2]`, and
// the arm is chosen by a flag that says nothing about either. Uniting the two
// keeps the lower bound; widening them drops it to zero and invents the
// release.
// CLEAN
#include <limits.h>
#include <stdlib.h>
#include <string.h>

static int reserve(char **slot, size_t want, int fixed) {
  char *grown;
  size_t size;
  if (slot == NULL || *slot == NULL)
    return -1;
  if (want == 0 || want > (size_t)(INT_MAX / 2))
    return -1;
  if (fixed)
    size = 64;
  else
    size = want;
  grown = realloc(*slot, size);
  if (grown == NULL) {
    free(*slot);
    *slot = NULL;
    return -1;
  }
  *slot = grown;
  return 0;
}

int main(void) {
  char *slot = malloc(8);
  if (slot == NULL)
    return 1;
  if (reserve(&slot, 128, 0) != 0)
    return 1;
  memset(slot, 0, 128);
  free(slot);
  return 0;
}
