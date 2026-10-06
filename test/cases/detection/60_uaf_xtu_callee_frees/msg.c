#include "msg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct msg *msg_new(const char *body, int prio) {
  struct msg *m = malloc(sizeof *m);
  if (!m)
    return NULL;
  m->body = strdup(body);
  m->prio = prio;
  return m;
}

int msg_send(struct msg *m) {
  int ok = printf("send[%d] %s\n", m->prio, m->body) > 0;
  free(m->body);
  free(m);
  return ok;
}
