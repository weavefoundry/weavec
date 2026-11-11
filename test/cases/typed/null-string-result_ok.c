// RFC 0042 §6: a null result declared WEAVEC_NULL_TERMINATED_OR_NULL is
// returned as it is; a bounded one is searched for its zero byte.
// CLEAN
// FLAGS: -O2 -fweavec-typed
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

static char *WEAVEC_NULL_TERMINATED_OR_NULL
copy(const char *WEAVEC_NULL_TERMINATED_OR_NULL s) {
  if (s == NULL)
    return NULL;
  size_t n = strlen(s) + 1;
  char *c = malloc(n);
  if (c != NULL)
    memcpy(c, s, n);
  return c;
}

int copied(const char *WEAVEC_NULL_TERMINATED_OR_NULL s) {
  char *c = copy(s);
  int r = c != NULL;
  free(c);
  return r;
}

WEAVEC_GUARDED int main(void) { return copied(NULL) == 0 && copied("x") == 1 ? 0 : 1; }
