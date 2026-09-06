// CWE-122: heap-based buffer overflow, a copy into an allocation one byte
// too small for the terminator (RFC 0012, *String facts*).
#include "recall.h"

void bad(const char *s) {
  char *d = malloc(strlen(s));
  if (!d)
    return;
  strcpy(d, s); // RECALL: out-of-bounds
  print_line(d);
  free(d);
}

void bad_length_first(const char *s) {
  size_t n = strlen(s);
  char *d = malloc(n);
  if (!d)
    return;
  strcpy(d, s); // RECALL: out-of-bounds
  print_line(d);
  free(d);
}

void bad_strdup_index(const char *s) {
  char *d = strdup(s);
  if (!d)
    return;
  d[strlen(s) + 1] = 0; // RECALL: out-of-bounds
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
