// RFC 0032 §3: the correct twin of stack-underflow_bug.c: the string has a last character.
// STAGE: S8
// CLEAN
// RUN-INPUT:
// ASAN
#include <string.h>
static void *same(void *p) { return p; }
static void *(*volatile hide)(void *) = same;
static void chop(char *s) {
  s[strlen(s) - 1] = 0; // GUARDED: spatial
}
int main(void) {
  char buf[8];
  buf[0] = 'x';
  buf[1] = 0;
  chop(hide(buf));
  return buf[0];
}
