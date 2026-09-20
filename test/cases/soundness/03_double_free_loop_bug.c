// Double free across loop iterations: cleanup inside loop, pointer not reset.
// ASAN
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argv;
  char *p = malloc(8);
  if (!p) return 1;
  for (int i = 0; i < argc + 1; i++) {
    if (i > 5) break;
    free(p); // BUG: double-free
  }
  return 0;
}
