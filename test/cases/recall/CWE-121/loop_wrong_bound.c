// CWE-121: stack-based buffer overflow, a loop bounded by the wrong constant
// (the source's size rather than the destination's).
#include "../recall.h"

void bad(void) {
  int src[100];
  int dst[50];
  for (int i = 0; i < 100; i++)
    dst[i] = src[i]; // BUG: out-of-bounds // TRAP: index
  print_int(dst[0]);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_int(int value) { (void)value; }
int main(void) {
  bad();
  return 0;
}
