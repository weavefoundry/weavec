// CWE-170: improper null termination, `strncpy` filling the whole buffer and
// a `%s` read of it (RFC 0012, *String facts*).
#include "recall.h"

void bad(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name);
  print_int((int)strlen(name)); // RECALL: out-of-bounds
}

void bad_puts(void) {
  char name[8];
  strncpy(name, "0123456789", sizeof name);
  puts(name); // RECALL: out-of-bounds
}

void bad_initialiser(void) {
  char name[4] = "abcd";
  printf("%s\n", name); // RECALL: out-of-bounds
}

void bad_memset(void) {
  char name[8];
  memset(name, 'x', sizeof name);
  print_int((int)strlen(name)); // RECALL: out-of-bounds
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
