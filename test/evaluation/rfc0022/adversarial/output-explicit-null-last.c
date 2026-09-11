/* RFC 0022: final-null output edges satisfy conditional bytes. */
#include <stdlib.h>
static void create(char **out, int fill) {
  if (fill) {
    *out = malloc(1);
    if (*out)
      **out = 7;
    return;
  }
  *out = 0;
}
int main(int argc, char **argv) {
  (void)argv;
  char *p = 0;
  create(&p, argc > 1);
  if (!p)
    return 0;
  int x = *p;
  free(p);
  return x;
}
