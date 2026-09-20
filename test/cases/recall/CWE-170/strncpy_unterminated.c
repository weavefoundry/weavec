// CWE-170: improper null termination, `strncpy` filling the whole buffer and
// a `%s` read of it (RFC 0012, *String facts*).
// RUN-INPUT: 1
// RUN-INPUT: 2
// RUN-INPUT: 3
// RUN-INPUT: 4
#include "../recall.h"

void bad(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name);
  print_int((int)strlen(name)); // BUG: out-of-bounds // TRAP: len
}

void bad_puts(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name);
  puts(name); // BUG: out-of-bounds // TRAP: len
}

void bad_initialiser(void) {
  char name[4] = "abcd";
  printf("%s\n", name); // BUG: out-of-bounds // TRAP: len
}

void bad_memset(void) {
  char name[8];
  memset(name, 'x', sizeof name);
  print_int((int)strlen(name)); // BUG: out-of-bounds // TRAP: len
}

void good(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name - 1);
  name[sizeof name - 1] = 0;
  puts(name);
  char other[8];
  strncpy(other, "0123456789", sizeof other);
  other[7] = 0;
  print_int((int)strlen(other));
  char zeroed[8];
  memset(zeroed, 0, sizeof zeroed);
  puts(zeroed);
  char fits[8];
  strncpy(fits, "abc", sizeof fits);
  puts(fits);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_int(int value) { (void)value; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good();
  if (which == 1) bad();
  if (which == 2) bad_puts();
  if (which == 3) bad_initialiser();
  if (which == 4) bad_memset();
  return 0;
}
