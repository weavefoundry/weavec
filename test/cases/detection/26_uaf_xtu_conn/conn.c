#include "conn.h"
#include <stdlib.h>
#include <string.h>

struct conn *conn_open(int fd, size_t cap) {
  struct conn *c = calloc(1, sizeof *c);
  if (!c)
    return NULL;
  c->rbuf = calloc(1, cap);
  if (!c->rbuf) {
    free(c);
    return NULL;
  }
  c->fd = fd;
  c->rcap = cap;
  return c;
}

size_t conn_feed(struct conn *c, const char *data) {
  size_t n = strlen(data);
  if (n > c->rcap - c->rlen - 1)
    n = c->rcap - c->rlen - 1;
  memcpy(c->rbuf + c->rlen, data, n);
  c->rlen += n;
  c->rbuf[c->rlen] = '\0';
  return n;
}

void conn_close(struct conn *c) {
  free(c->rbuf);
  free(c);
}
