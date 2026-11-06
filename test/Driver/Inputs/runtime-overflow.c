/* A heap block written up to argv[1] bytes: runtime-link.c's foreign link. */
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  char *p = malloc(4);
  if (p == NULL || argc < 2)
    return 2;
  memset(p, 1, (size_t)atoi(argv[1]));
  free(p);
  return 0;
}
