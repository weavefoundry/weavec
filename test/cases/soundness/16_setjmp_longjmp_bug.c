// setjmp/longjmp: free on the first pass, use on the second return.
// ASAN
#include <setjmp.h>
#include <stdlib.h>
static jmp_buf env;
static void fail(void) { longjmp(env, 1); }
int main(void) {
  char *volatile p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  if (setjmp(env) != 0) {
    return p[0]; // BUG: use-after-free // NOT-PROVEN: temporal
  }
  free(p);
  fail();
  return 0;
}
