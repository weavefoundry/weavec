// Beyond 32 tracked cells: free element 40, then read it.
// ASAN
#include <stdlib.h>
int main(void) {
  char *items[64];
  for (int i = 0; i < 64; i++) { items[i] = malloc(4); if (!items[i]) return 1; items[i][0] = 1; }
  free(items[40]);
  int r = items[40][0]; // BUG: use-after-free
  for (int i = 0; i < 64; i++) if (i != 40) free(items[i]);
  return r;
}
