#include "sbuf.h"
#include <stdlib.h>
#include <string.h>

int sbuf_init(struct sbuf *b, size_t cap) {
  b->data = malloc(cap);
  b->len = 0;
  b->cap = b->data ? cap : 0;
  if (b->data)
    b->data[0] = '\0';
  return b->data ? 0 : -1;
}

int sbuf_append(struct sbuf *b, const char *s) {
  size_t n = strlen(s);
#ifdef FIX
  if (b->len + n + 1 > b->cap) {
    size_t ncap = (b->len + n + 1) * 2;
    char *nd = realloc(b->data, ncap);
    if (!nd)
      return -1;
    b->data = nd;
    b->cap = ncap;
  }
#endif
  memcpy(b->data + b->len, s, n + 1); // STOP
  b->len += n;
  return 0;
}

void sbuf_free(struct sbuf *b) {
  free(b->data);
  b->data = NULL;
}
