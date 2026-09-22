// CWE-122: heap-based buffer overflow, a copy into an allocation one byte
// too small for the terminator (RFC 0012, *String facts*).
// RUN-INPUT: 1
// RUN-INPUT: 2
// RUN-INPUT: 3
#include "../recall.h"

void bad(const char *s) {
  char *d = malloc(strlen(s));
  if (!d)
    return;
  strcpy(d, s); // BUG: out-of-bounds // TRAP: len
  print_line(d);
  free(d);
}

void bad_length_first(const char *s) {
  size_t n = strlen(s);
  char *d = malloc(n);
  if (!d)
    return;
  strcpy(d, s); // BUG: out-of-bounds // TRAP: len
  print_line(d);
  free(d);
}

void bad_strdup_index(const char *s) {
  char *d = strdup(s);
  if (!d)
    return;
  d[strlen(s) + 1] = 0; // BUG: out-of-bounds // TRAP: index
  print_line(d);
  free(d);
}

void good(const char *s) {
  char *d = malloc(strlen(s) + 1);
  if (!d)
    return;
  strcpy(d, s);
  print_line(d);
  free(d);
  size_t n = strlen(s);
  char *e = malloc(n + 1);
  if (!e)
    return;
  strcpy(e, s);
  e[n] = 0;
  print_line(e);
  free(e);
}

// Driver (RFC 0030 section 17.2): executes the defect so the runtime oracle can observe the check.
void print_line(const char *s) { (void)s; }
int main(int argc, char **argv) {
  int which = argc > 1 ? argv[1][0] - '0' : 0;
  good("hello");
  if (which == 1) bad("hello");
  if (which == 2) bad_length_first("hello");
  if (which == 3) bad_strdup_index("hello");
  return 0;
}
