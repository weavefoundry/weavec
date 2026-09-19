// RFC 0030 §7.4: a flexible-array struct allocated smaller than sizeof is enough for its members.
// STAGE: S3
// The object width of a struct with a flexible array member is the member's offset, not
// sizeof. offsetof(struct S, data) + 3 bytes (12 here, sizeof is 16) cover p->a and
// p->tag, and data[0..2] is the rest of the allocation. The conversion of malloc's result
// is not a required position, so nothing is checked against sizeof. No error, no trap.
// CLEAN
// ASAN
#include <stddef.h>
#include <stdlib.h>

struct S { long long a; char tag; char data[]; };

int main(void) {
  struct S *p = malloc(offsetof(struct S, data) + 3);
  if (p == NULL) return 1;
  p->a = 1;
  p->tag = 't';
  for (int i = 0; i < 3; i++) p->data[i] = (char)('a' + i);
  int r = p->data[2] == 'c' && p->a == 1 && p->tag == 't' ? 0 : 1;
  free(p);
  return r;
}
