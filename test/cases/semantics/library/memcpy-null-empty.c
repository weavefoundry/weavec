// RFC 0030 §8.3: an empty vector's memcpy(NULL, NULL, 0) runs without a trap.
// STAGE: S4
// Zero-length memcpy accepts null: both pointer arguments are null-if-zero, and their null
// facets use the zero-length form of the nonnull check, which traps only when the length
// is non-zero. Copying an empty vector, whose data pointers are null, runs. No error, no
// trap.
// CLEAN
// ASAN
#include <stddef.h>
#include <string.h>

struct vec { int *data; size_t len; };

static void copy(struct vec *d, const struct vec *s) {
  memcpy(d->data, s->data, s->len * sizeof *s->data);
  d->len = s->len;
}

int main(void) {
  struct vec a = {NULL, 0};
  struct vec b = {NULL, 0};
  copy(&a, &b);
  return (int)a.len;
}
