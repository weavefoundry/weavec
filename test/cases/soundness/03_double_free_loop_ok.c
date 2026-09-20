// CLEAN
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argv;
  char *p = malloc(8);
  if (!p) return 1;
  for (int i = 0; i < argc + 1; i++) {
    if (i > 5) break;
    free(p);
    p = NULL;
  }
  return 0;
}
