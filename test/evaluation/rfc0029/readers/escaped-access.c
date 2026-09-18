#include "cursor.h"
static unsigned peek(const struct cursor *c) { return c->data[c->position]; }
int main(void) {
  const unsigned char data[] = {1, 2};
  struct cursor c = {0, 0, data, sizeof data, 3};
  return (int)peek(&c);
}
