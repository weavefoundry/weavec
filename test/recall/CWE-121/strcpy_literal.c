// CWE-121: stack-based buffer overflow, a string literal copied into a
// smaller array (RFC 0012, *String facts*).
#include "recall.h"

void bad(void) {
  char buf[8];
  strcpy(buf, "0123456789"); // RECALL: out-of-bounds
  print_line(buf);
}

void bad_strcat(void) {
  char buf[8];
  strcpy(buf, "0123");
  strcat(buf, "45678"); // RECALL: out-of-bounds
  print_line(buf);
}

void bad_sprintf(int x) {
  char buf[4];
  sprintf(buf, "%d!!!", x); // RECALL: out-of-bounds
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
