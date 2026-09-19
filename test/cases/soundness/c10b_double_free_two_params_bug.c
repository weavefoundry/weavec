// ASAN
#include <stdlib.h>
static void cleanup(char *a, char *b) { free(a); free(b); } // BUG: double-free
int main(void) {
  char *p = malloc(8);
  cleanup(p, p);
  return 0;
}
