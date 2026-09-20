// RFC 0030 §4 "Unresolved, inexpressible": the extent is known but has no C name at the access.
// STAGE: S6
// The extent of 'd' is 'b->cap' of the old 'b'. After 'b = other' the engine still knows it
// as a snapshot place, but a check at d[i] cannot name it (§10.3 rule 4).
#include <stddef.h>
#include "../Inputs/rfc0030.h"

struct buf { char *WEAVEC_COUNTED_BY(cap) data; size_t cap; };

char get(struct buf *b, struct buf *other, size_t i) {
  char *d = b->data;
  b = other; /* the extent was 'b->cap' of the old 'b' */
  return d[i]; // UNRESOLVED: spatial:inexpressible
}
