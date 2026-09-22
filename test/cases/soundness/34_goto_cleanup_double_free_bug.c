// RUN-INPUT: x
// ASAN
#include <stdlib.h>
int work(int fail) {
  char *a = malloc(8), *b = NULL;
  if (!a) return -1;
  b = malloc(8);
  if (!b) goto err;
  if (fail) { free(a); goto err; }
  free(b);
  free(a);
  return 0;
err:
  free(b);
  free(a); // BUG: double-free
  return -1;
}
int main(int argc, char **argv) { (void)argv; return work(argc > 1); }
