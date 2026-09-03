/* A `realloc` wrapper and an error-returning consumer defined in another
 * unit: their outcome-conditional summaries cross the unit boundary (RFC
 * 0006, RFC 0005). */
#include "../../Inputs/prelude.h"

char *grow(char *p, size_t n) {
  char *q = realloc(p, n);
  if (!q)
    return NULL;
  return q;
}

int try_take(char *p, int c) {
  if (c) {
    free(p);
    return 0;
  }
  return -1;
}
