// RFC 0033 §1: `(void)x` discards a value without reading it; it is no use of an
// uninitialised variable (libuv's fs.c).
// STAGE: S1
// CLEAN
// RUN-INPUT:
#include <stddef.h>
static int maybe(int flag, char **out) {
  char *tmp;
  (void)tmp;
  if (flag) { *out = NULL; return 1; }
  return 0;
}
int main(void) {
  char *p = (char *)"x";
  return maybe(1, &p) == 1 && p == NULL ? 0 : 1;
}
