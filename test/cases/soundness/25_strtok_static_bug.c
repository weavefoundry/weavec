// strtok keeps a pointer into a buffer that is freed before the next call.
#include <stdlib.h>
#include <string.h>
int main(void) {
  char *s = strdup("a,b,c");
  if (!s) return 1;
  char *t = strtok(s, ",");
  free(s);
  t = strtok(NULL, ",");
  return t ? t[0] : 0; // TRAP
}
