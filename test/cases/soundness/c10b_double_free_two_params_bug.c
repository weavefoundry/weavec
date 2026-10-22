// ASAN
#include <stdlib.h>
static void cleanup(char *a, char *b) { free(a); free(b); } // TRAP: invalid-release
int main(void) {
  char *p = malloc(8);
  cleanup(p, p);
  return 0;
}
