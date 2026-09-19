// Iterator invalidation: pointer into vector kept across growth.
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argv;
  int *v = malloc(4 * sizeof *v);
  if (!v) return 1;
  for (int i = 0; i < 4; i++) v[i] = i;
  int *it = &v[1];
  size_t cap = 4u + (size_t)argc * 4096u;
  int *nv = realloc(v, cap * sizeof *nv);
  if (!nv) { free(v); return 1; }
  v = nv;
  *it = 42; // BUG: use-after-move
  int r = v[1];
  free(v);
  return r;
}
