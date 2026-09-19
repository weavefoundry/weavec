#include "cursor.h"
int take(struct cursor *c, unsigned char *out) {
  if (c->position >= c->end) return 0;
  *out = c->data[c->position];
  c->position++;
  return 1;
}
unsigned consume(struct cursor *c) {
  unsigned sum = 0;
  while (c->position < c->end) {
    sum += c->data[c->position]; // NEUTRALISED: zero-init
    c->position++;
  }
  return sum;
}
