/* RFC 0022: every reaching target must establish its obligations. */
#include <stdlib.h>
#include <string.h>
static void *initialized(size_t n) {
  return calloc(n, 1);
}
int main(int argc, char **argv) {
  (void)argv;
  void *(*fn)(size_t) = argc > 1 ? malloc : initialized;
  char *p = fn(8);
  if (!p)
    return 0;
  int x = p[7];
  free(p);
  return x;
}
