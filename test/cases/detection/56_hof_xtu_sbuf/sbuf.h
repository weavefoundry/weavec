#ifndef SBUF_H
#define SBUF_H
#include <stddef.h>

struct sbuf {
  char *data;
  size_t len, cap;
};

int sbuf_init(struct sbuf *b, size_t cap);
int sbuf_append(struct sbuf *b, const char *s);
void sbuf_free(struct sbuf *b);
#endif
