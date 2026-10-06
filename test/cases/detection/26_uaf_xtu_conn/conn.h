#ifndef CONN_H
#define CONN_H
#include <stddef.h>

struct conn {
  int fd;
  char *rbuf;
  size_t rlen, rcap;
};

struct conn *conn_open(int fd, size_t cap);
size_t conn_feed(struct conn *c, const char *data);
void conn_close(struct conn *c);
#endif
