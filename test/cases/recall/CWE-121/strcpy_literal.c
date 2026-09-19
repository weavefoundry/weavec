// CWE-121: stack-based buffer overflow, a string literal copied into a
// smaller array (RFC 0012, *String facts*).
// RUN-INPUT: 1
// RUN-INPUT: 2
// RUN-INPUT: 3
#include "../recall.h"

void bad(void) {
  char buf[8];
  strcpy(buf, "0123456789"); // BUG: out-of-bounds // TRAP: len
  print_line(buf);
}

void bad_strcat(void) {
  char buf[8];
  strcpy(buf, "0123");
  strcat(buf, "45678"); // BUG: out-of-bounds // TRAP: len
  print_line(buf);
}

void bad_sprintf(int x) {
  char buf[4];
  sprintf(buf, "%d!!!", x); // BUG: out-of-bounds // TRAP: len
  print_line(buf);
}

void good(void) {
  char buf[8];
  strcpy(buf, "0123456");
  strcat(buf, "");
  print_line(buf);
  char two[8];
  strcpy(two, "012");
  strcat(two, "3456");
  print_line(two);
  char fmt[8];
  snprintf(fmt, sizeof fmt, "%d", 12345678);
  print_line(fmt);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_line(const char *s) { (void)s; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad();
  if (which == 2) bad_strcat();
  if (which == 3) bad_sprintf(12345);
  return 0;
}
