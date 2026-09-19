// Engine pin converted from test/Analysis/rfc0008-release.c; markers are the v0.10.0 golden diagnostics.
// RFC 0008, *Invalid releases*: a releaser is handed a pointer to a stack or
// static object, to a string literal, or into the middle of an allocation.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The RFC's snippets that must be reported.

static char table[16];

void stack(void) {
  char buf[8];
  char *p = buf;
  free(p); // BUG: invalid-release
}

void object(void) {
  int x;
  int *p = &x;
  free(p); // BUG: invalid-release
}

void global(void) {
  free(table); // BUG: invalid-release
}

void literal(void) {
  char *s = "hello";
  free(s); // BUG: invalid-release
}

void interior(void) {
  char *p = malloc(8);
  if (!p)
    return;
  char *q = p + 1;
  free(q); // BUG: invalid-release
}

void searched(const char *s) {
  char *p = strdup(s);
  if (!p)
    return;
  char *q = strchr(p, 'x');
  if (!q) {
    free(p);
    return;
  }
  free(q); // BUG: invalid-release
}

void arithmetic(void) {
  char *p = malloc(8);
  if (!p)
    return;
  free(p + 1); // BUG: invalid-release
}

// Clean: heap objects released at their start, `p + 0`, and a pointer that
// walks forward and back again is the checker's business elsewhere.
void fine(void) {
  char *p = malloc(8);
  if (!p)
    return;
  char *q = p;
  free(q + 0);
  char *s = strdup("x");
  free(s);
}

// Clang itself warns about `free(table)`; WeaveC reports it too, once.
