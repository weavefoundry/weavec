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
    // RFC 0030 §7.3: 'c' is Single by A1, so the loads of c->data and c->position
    // on the next line are proven; the subscript, the false proof, is not.
    sum += c->data[c->position]; // UNRESOLVED: spatial:unknown-extent
    c->position++;
  }
  return sum;
}
