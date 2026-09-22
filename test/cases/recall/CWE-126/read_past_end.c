// CWE-126: buffer over-read, a constant index one past the end.
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

void bad(void) {
  int data[10];
  memset(data, 0, sizeof data);
  print_int(data[10]); // BUG: out-of-bounds // TRAP: index
}

void bad_heap(void) {
  int *data = malloc(10 * sizeof *data);
  if (!data)
    return;
  memset(data, 0, 10 * sizeof *data);
  print_int(data[10]); // BUG: out-of-bounds // TRAP: index
  free(data);
}

void good(void) {
  int data[10];
  memset(data, 0, sizeof data);
  print_int(data[9]);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_int(int value) { (void)value; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad();
  if (which == 2) bad_heap();
  return 0;
}
