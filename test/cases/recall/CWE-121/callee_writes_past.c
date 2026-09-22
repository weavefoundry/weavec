// CWE-121: stack-based buffer overflow in a callee that assumes a size the
// caller's buffer does not have.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

static void fill_eight(char *buf) {
  for (int i = 0; i < 8; i++)
    buf[i] = 'A';
}

static void fill_n(int *buf, int n) {
  for (int i = 0; i < n; i++)
    buf[i] = i;
}

void bad(void) {
  char small[4];
  fill_eight(small); // BUG: out-of-bounds // TRAP: len
  print_bytes(small, 4);
}

void bad_symbolic(void) {
  int ints[8];
  fill_n(ints, 16); // BUG: out-of-bounds // TRAP: len
  print_int(ints[0]);
}

void good(void) {
  char big[8];
  fill_eight(big);
  int ints[8];
  fill_n(ints, 8);
  print_bytes(big, 8);
  print_int(ints[0]);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_int(int value) { (void)value; }
void print_bytes(const void *p, size_t n) { (void)p; (void)n; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad();
  if (which == 2) bad_symbolic();
  return 0;
}
