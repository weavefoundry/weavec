// CWE-121: stack-based buffer overflow through memcpy with the source's size.
#include "../recall.h"

void bad(void) {
  char src[100];
  char dst[50];
  memset(src, 'A', 100);
  memcpy(dst, src, 100); // BUG: out-of-bounds // TRAP: len
  print_bytes(dst, 50);
}

void good(void) {
  char src[100];
  char dst[50];
  memset(src, 'A', 100);
  memcpy(dst, src, 50);
  print_bytes(dst, 50);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(void) {
  good();
  bad();
  return 0;
}
