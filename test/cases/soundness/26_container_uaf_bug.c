// Array of owned pointers: free all in a loop, then read an element.
// ASAN
#include <stdlib.h>
int main(void) {
  char *items[4];
  for (int i = 0; i < 4; i++) { items[i] = malloc(4); if (!items[i]) return 1; items[i][0] = (char)i; }
  for (int i = 0; i < 4; i++) free(items[i]);
  return items[2][0]; // BUG: use-after-free
}
