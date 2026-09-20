// CWE-121: stack-based buffer overflow through a pointer stepped past the end.
#include "../recall.h"

void bad(void) {
  char buf[8];
  char *p = buf + 4;
  p[4] = 0; // BUG: out-of-bounds // TRAP: span
  print_bytes(buf, 8);
}

void good(void) {
  char buf[8];
  char *p = buf + 4;
  p[3] = 0;
  print_bytes(buf, 8);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(void) {
  good();
  bad();
  return 0;
}
