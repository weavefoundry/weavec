#ifndef STRBUF_H
#define STRBUF_H
#include <stddef.h>

struct strbuf {
  char *data;
  size_t len, cap;
};

int strbuf_init(struct strbuf *sb, size_t cap);
int strbuf_add(struct strbuf *sb, const char *s);
void strbuf_release(struct strbuf *sb);
#endif
