// CWE-126: buffer over-read, a loop that reads one too many.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

int bad(void) {
  int data[10];
  int sum = 0;
  memset(data, 0, sizeof data);
  for (int i = 0; i <= 10; i++)
    sum += data[i]; // BUG: out-of-bounds // TRAP: index
  return sum;
}

void bad_memcpy_source(void) {
  char small[8];
  char big[16];
  memset(small, 0, 8);
  memcpy(big, small, 16); // BUG: out-of-bounds // TRAP: len
  print_bytes(big, 16);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  if (which == 1) bad();
  if (which == 2) bad_memcpy_source();
  return 0;
}
