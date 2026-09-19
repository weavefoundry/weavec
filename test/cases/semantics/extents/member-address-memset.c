// RFC 0030 §7.4: memset through the address of a first member covers the whole object.
// STAGE: S3
// '&s->first' has the extent of the complete object, so memset(&s->first, 0, sizeof *s) is
// in bounds, for a parameter (Single under A1: sizeof(struct S) bytes) and for a local (an
// exact extent). The member's own four bytes are not the bound. No error, no trap.
// CLEAN
// ASAN
#include <string.h>

struct S { int first; int second; char *name; };

void reset(struct S *s) { memset(&s->first, 0, sizeof *s); }

int main(void) {
  struct S a = {1, 2, "a"};
  struct S b = {3, 4, "b"};
  memset(&a.first, 0, sizeof a);
  reset(&b);
  return a.second == 0 && b.name == NULL ? 0 : 1;
}
