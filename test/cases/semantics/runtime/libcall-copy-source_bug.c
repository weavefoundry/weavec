// RFC 0034 §5.2 and amendments: a copy whose length no term can state
// (`*n` behind a parameter, here) calls the checked wrapper, which checks the
// bytes behind the source as well as the room behind the destination. The
// erase moves one element too many, reading past the array.
// RUN-INPUT:
// ASAN
#include <stdlib.h>
#include <string.h>
static void erase(int *a, size_t *n, size_t i) {
  memmove(&a[i], &a[i + 1], (*n - i) * sizeof *a); // BUG: out-of-bounds // TRAP
  --*n;
}
int main(void) {
  size_t n = 5;
  int *a = malloc(5 * sizeof *a);
  if (!a)
    return 1;
  for (int i = 0; i < 5; i++)
    a[i] = i;
  erase(a, &n, 1);
  int r = a[1];
  free(a);
  return r == 2 ? 0 : 1;
}
