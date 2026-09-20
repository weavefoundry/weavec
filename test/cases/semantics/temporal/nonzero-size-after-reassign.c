// RFC 0009, *Scalar facts in the state*: what a comparison leaves on a
// reassigned parameter must survive the arithmetic that follows it.
// STAGE: S8
// RFC 0030 §8.2 makes `realloc(p, 0)` a release of `p`, so `grow`'s
// `if (grown == NULL) free(b->data);` is a double free exactly when the size
// can be zero. Here it cannot: the fall-through of `needed <= b->length` has
// `needed > b->length >= 0`, so `needed >= 1`, and the early return bounds it
// by `INT_MAX / 2`, so `needed * 2` is between 2 and `INT_MAX - 1`.
// `grow` reassigns its own parameter, so the engine also carries `needed`
// symbolically against an entry snapshot that no later test narrows; the
// interval left on the variable itself is the only thing that proves the
// product non-zero, and evaluating the product must keep both.
// CLEAN
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct buffer {
  char *data;
  size_t length;
  size_t offset;
};

static int grow(struct buffer *b, size_t needed) {
  char *grown;
  if (b == NULL || b->data == NULL)
    return -1;
  if (needed > (size_t)(INT_MAX / 4))
    return -1;
  needed += b->offset + 1;
  if (needed <= b->length)
    return 0;
  if (needed > (size_t)(INT_MAX / 2))
    return -1;
  grown = realloc(b->data, needed * 2);
  if (grown == NULL) {
    free(b->data);
    b->data = NULL;
    b->length = 0;
    return -1;
  }
  b->data = grown;
  b->length = needed * 2;
  return 0;
}

int main(void) {
  struct buffer b;
  b.data = malloc(8);
  if (b.data == NULL)
    return 1;
  b.length = 8;
  b.offset = 0;
  if (grow(&b, 40) != 0)
    return 1;
  memset(b.data, 0, b.length);
  free(b.data);
  return 0;
}
