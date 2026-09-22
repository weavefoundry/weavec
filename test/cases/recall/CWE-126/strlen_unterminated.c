// CWE-126: buffer over-read, a terminator-seeking read of an array that has
// no terminator (RFC 0012, *String facts*).
// RUN-INPUT: 1
// RUN-INPUT: 2
#include "../recall.h"

int bad(void) {
  char data[4] = {'a', 'b', 'c', 'd'};
  return (int)strlen(data); // BUG: out-of-bounds // TRAP: len
}

void bad_copy(void) {
  char data[4] = {'a', 'b', 'c', 'd'};
  char out[16];
  strcpy(out, data); // BUG: out-of-bounds // TRAP: len
  print_line(out);
}

int good(void) {
  char data[4] = {'a', 'b', 'c', 0};
  char out[16];
  strcpy(out, data);
  print_line(out);
  return (int)strlen(data);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_line(const char *s) { (void)s; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad();
  if (which == 2) bad_copy();
  return 0;
}
