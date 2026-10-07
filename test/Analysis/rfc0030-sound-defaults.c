// RFC 0030 §5 and §3.1: the defaults for code the analysis cannot see. None
// of them is a diagnostic: the facets are not proven (RFC 0035 §8).
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=QUIET %s
#include <stdlib.h>
#include <sys/ioctl.h>

// QUIET-NOT: {{warning|error}}:

void consume(char *p);

// §5.1: 'consume' may have released or kept 'p'; the release after it is
// unresolved too, and replaces its record.
int unknown(void) {
  char *p = malloc(8);
  if (!p)
    return 0;
  consume(p);
  int v = p[0];
  free(p);
  return v;
}

// §5.2: a platform function without a table entry borrows its arguments.
int system_api(int fd) {
  struct winsize ws;
  if (ioctl(fd, TIOCGWINSZ, &ws) == -1)
    return 80;
  return ws.ws_col;
}

// §5.7: inline assembly handed `p` is unknown code.
int assembly(void) {
  char *p = malloc(8);
  if (!p)
    return 0;
  __asm__ volatile("" : : "r"(p) : "memory");
  int v = p[0];
  free(p);
  return v;
}

// §3.1: `b` may be the released `a` (both point to char).
void two(char *a, char *b) {
  free(a);
  b[0] = 1;
}

// §11: without zero-initialisation a pointer that may be uninitialised may
// hold garbage no null check catches.
int deref(int c, int *q) {
  int *p;
  if (c)
    p = q;
  return *p;
}
