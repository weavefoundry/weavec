static void fill(unsigned char *p) {
  unsigned char *out = p;
  *out++ = 7;
  *out = 0;
}
#include <stdlib.h>
int main(void) {
  unsigned char *bytes = malloc(2);
  if (!bytes) return 0;
  bytes[0] = bytes[1] = 1;
  fill(bytes);
  if (*bytes == 0) bytes[2] = 1;
  free(bytes); return 0;
}
