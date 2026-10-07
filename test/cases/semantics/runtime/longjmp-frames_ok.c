// RFC 0032 §4.3: a longjmp skips the cleanups that leave stack objects; the entries of the frames it abandoned are dropped when the setjmp returns.
// STAGE: S4
// 'thrower' registers 'scratch' and jumps out. 'reader' then runs where that frame was, with
// a buffer of its own; a stale entry for 'scratch' would give its guard the wrong object.
// CLEAN
// RUN-INPUT:
#include <setjmp.h>
#include <string.h>
static jmp_buf env;
static int peek(const char *p, int i) { return p[i]; }
static void thrower(int depth) {
  char scratch[24];
  memset(scratch, depth, sizeof scratch);
  if (peek(scratch, 23) != depth) return;
  if (depth == 0) longjmp(env, 1);
  thrower(depth - 1);
}
static int reader(void) {
  char small[4] = {1, 2, 3, 4};
  char big[64];
  memset(big, 9, sizeof big);
  return peek(small, 3) + peek(big, 63);
}
int main(void) {
  int total = 0;
  for (int round = 0; round < 3; round++) {
    if (setjmp(env) == 0)
      thrower(3);
    total += reader();
  }
  return total != 3 * 13;
}
