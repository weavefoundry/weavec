// CLEAN
// ASAN
#include <stdlib.h>
int main(void) {
  char *items[4] = {0};
  for (int i = 0; i < 4; i++) { items[i] = malloc(4); if (!items[i]) goto out; items[i][0] = (char)i; }
  int r = items[2][0];
  for (int i = 0; i < 4; i++) free(items[i]);
  return r;
out:
  for (int i = 0; i < 4; i++) free(items[i]);
  return 1;
}
