// CWE-121: stack-based buffer overflow, a counted loop that runs one too far.
#include "../recall.h"

void bad(void) {
  int data[10];
  for (int i = 0; i <= 10; i++)
    data[i] = i; // BUG: out-of-bounds // TRAP: index
  print_int(data[0]);
}

void good(void) {
  int data[10];
  for (int i = 0; i < 10; i++)
    data[i] = i;
  print_int(data[0]);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_int(int value) { (void)value; }
int main(void) {
  good();
  bad();
  return 0;
}
