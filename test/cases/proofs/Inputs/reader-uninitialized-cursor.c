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
    // The salvaged false proof is the subscript, as in reader-short. A
    // not-proven marker cannot pin it, since it holds for the whole line and
    // the loads of c->data and c->position on this line are proven (§7.3).
    sum += c->data[c->position]; // NEUTRALISED: zero-init // UNRESOLVED: spatial:unknown-extent
    c->position++;
  }
  return sum;
}
