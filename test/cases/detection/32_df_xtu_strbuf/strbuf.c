#include "strbuf.h"
#include <stdlib.h>
#include <string.h>

int strbuf_init(struct strbuf *sb, size_t cap) {
  sb->data = calloc(1, cap);
  sb->len = 0;
  sb->cap = sb->data ? cap : 0;
  return sb->data ? 0 : -1;
}

int strbuf_add(struct strbuf *sb, const char *s) {
  size_t n = strlen(s);
  if (sb->len + n + 1 > sb->cap) {
    size_t ncap = (sb->len + n + 1) * 2;
    char *nd = realloc(sb->data, ncap);
    if (!nd)
      return -1;
    sb->data = nd;
    sb->cap = ncap;
  }
  memcpy(sb->data + sb->len, s, n + 1);
  sb->len += n;
  return 0;
}

void strbuf_release(struct strbuf *sb) {
  free(sb->data);
#ifdef FIX
  sb->data = NULL;
#endif
  sb->len = sb->cap = 0;
}
