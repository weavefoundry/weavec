// CWE-121: stack-based buffer overflow in a callee that assumes a size the
// caller's buffer does not have.
#include "recall.h"

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
  fill_eight(small); // RECALL: out-of-bounds
  print_bytes(small, 4);
}

void bad_symbolic(void) {
  int ints[8];
  fill_n(ints, 16); // RECALL: out-of-bounds
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
