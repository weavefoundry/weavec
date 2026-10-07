// RFC 0031 *Implementation amendments* (realloc of zero bytes): a failed
// realloc keeps the buffer in a zero-initialised build.
// STAGE: S7
// 'append' grows the buffer with realloc and returns on failure, leaving the old
// buffer in place. The zero-initialisation wrapper never asks realloc for zero bytes, so
// the null class keeps the argument: after any number of appends 'b.data' is live on
// every path, and no call boundary sees a pointer that may be gone (the temporal facets
// of the buffer's uses rest on nothing a boundary broke). Linenoise's 'abAppend'.
// CLEAN
// RUN-INPUT:
#include <stdlib.h>
#include <string.h>

struct buf {
  char *data;
  int len;
};

static void append(struct buf *b, const char *s, int n) {
  char *grown = realloc(b->data, (size_t)(b->len + n));
  if (grown == NULL)
    return;
  memcpy(grown + b->len, s, (size_t)n);
  b->data = grown;
  b->len += n;
}

int main(void) {
  struct buf b = {NULL, 0};
  append(&b, "ab", 2);
  append(&b, "cd", 2);
  append(&b, "e", 1);
  int ok = b.len == 5 && b.data[4] == 'e';
  free(b.data);
  return ok ? 0 : 1;
}
