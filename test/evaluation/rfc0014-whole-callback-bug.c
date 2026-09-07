// RFC 0014: independently identified pointer-identity evaluation.
#include <stdlib.h>
#include <string.h>
void invoke(void (*)(void *), void *);
static void drop(void *p) { free(p); }
void bad(int *p) { invoke(drop, p);
  *p = 1; // BUG: whole-callback
}
