// RFC 0032 §3, amendment 21: a negative index from the start of a stack object leaves the
// object, and traps, unless its frame has automatic storage the object list does not know.
// 'chop' on an empty string writes buf[-1].
// STAGE: S8
// RUN-INPUT:
// ASAN
#include <string.h>
static void *same(void *p) { return p; }
static void *(*volatile hide)(void *) = same;
static void chop(char *s) {
  s[strlen(s) - 1] = 0; // BUG: out-of-bounds // TRAP: object // GUARDED: spatial
}
int main(void) {
  char buf[8];
  buf[0] = 0;
  chop(hide(buf));
  return buf[0];
}
