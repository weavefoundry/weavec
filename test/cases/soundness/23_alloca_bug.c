// alloca: overrun and escape from frame.
// ASAN
#include <alloca.h>
static char *make(void) {
  char *p = alloca(8);
  p[0] = 1;
  return p; // BUG: lifetime-too-short
}
int main(void) {
  char *q = alloca(8);
  q[8] = 1; // BUG: out-of-bounds
  char *d = make();
  return d[0];
}
