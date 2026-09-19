// RFC 0030 §9.1: a wrapper's free on the path that returns a non-null local is keyed to that class.
// STAGE: S7
// In 'my_realloc' the free carries the guard 'm nonnull and ptr nonnull'. At 'return m' the
// conjunct on the local 'm' becomes the result class nonnull, so the summary frees 'ptr'
// only when the result is non-null. The caller's test selects the class exactly: on the
// null path the old block is still live and is freed once. No error, no warning, no trap.
// CLEAN
// ASAN
#include <stdlib.h>
#include <string.h>

void *my_realloc(void *ptr, size_t old, size_t n) {
  void *m = malloc(n);
  if (m && ptr) { memcpy(m, ptr, old < n ? old : n); free(ptr); }
  return m;
}

int grow(char **buf, size_t old, size_t n) {
  char *q = my_realloc(*buf, old, n);
  if (q == NULL) {
    free(*buf);
    *buf = NULL;
    return -1;
  }
  *buf = q;
  return 0;
}

int main(void) {
  char *b = malloc(4);
  if (b == NULL) return 1;
  memcpy(b, "abc", 4);
  if (grow(&b, 4, 16) != 0) return 1;
  int same = strcmp(b, "abc") == 0;
  free(b);
  return same ? 0 : 1;
}
