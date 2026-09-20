// CWE-121: stack-based buffer overflow, a constant index one past the end.
#include "../recall.h"

void bad(void) {
  char buf[10];
  buf[10] = 'A'; // BUG: out-of-bounds // TRAP: index
}

void good(void) {
  char buf[10];
  buf[9] = 'A';
  print_bytes(buf, 10);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(void) {
  good();
  bad();
  return 0;
}
