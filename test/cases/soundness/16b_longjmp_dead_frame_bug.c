// longjmp into a frame that has already returned.
// TOOL
#include <setjmp.h>
static jmp_buf env;
static int arm(void) {
  char local[16];
  local[0] = 1;
  if (setjmp(env)) return local[0]; // BUG: lifetime-too-short // MISS: a longjmp into a frame that has returned is not detected (RFC 0035, assumption A4)
  return 0;
}
int main(void) {
  arm();
  longjmp(env, 1);
}
