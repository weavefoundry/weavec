// CWE-126: buffer over-read, a terminator-seeking read of an array that has
// no terminator (RFC 0012, *String facts*).
#include "recall.h"

int bad(void) {
  char data[4] = {'a', 'b', 'c', 'd'};
  return (int)strlen(data); // RECALL: out-of-bounds
}

void bad_copy(void) {
  char data[4] = {'a', 'b', 'c', 'd'};
  char out[16];
  strcpy(out, data); // RECALL: out-of-bounds
  print_line(out);
}

int good(void) {
  char data[4] = {'a', 'b', 'c', 0};
  char out[16];
  strcpy(out, data);
  print_line(out);
  return (int)strlen(data);
}
