// RFC 0033 §1: the same callee handed a smaller array is still a definite violation.
// STAGE: S1
// TOOL
#include <string.h>
static void clear_ints(int *p) { memset(p, 0, 4 * sizeof(int)); }
int main(void) {
  int a[3] = {1, 2, 3};
  clear_ints(a); // BUG: out-of-bounds definite
  return a[2];
}
