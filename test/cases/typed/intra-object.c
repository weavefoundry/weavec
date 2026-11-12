// RFC 0042 §4: an index past an array field stops at the field, inside its
// object, where the shadow sees only the block.
// FLAGS: -O2 -fweavec-safe
#include <weavec.h>
#include <stdlib.h>

struct record { char name[8]; struct record *next; };

char letter(const struct record *r, size_t i) {
  return r->name[i]; // TRAP: index-out-of-bounds
}

WEAVEC_GUARDED int main(void) {
  struct record *r = calloc(1, sizeof *r);
  return letter(r, 8);
}
