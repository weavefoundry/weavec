// RFC 0030 §3.1: two pointers loaded from owning places are distinct, so 'free(s->a); free(s->b)' stays proven.
// STAGE: S7
// 'pair.a' and 'pair.b' are owning slots (a value loaded from each is released), and the
// owner-uniqueness assumption (§9.4) keeps their objects distinct, so the second release is
// not may-alias-released. 'int' is not a character type and is not compatible with
// 'struct pair', so the accesses through 's' are distinct from the released objects too.
// The fields are cleared, so no boundary is left holding a released pointer: no temporal
// facet of the unit is unresolved.
// CLEAN
// EXPECT-LEDGER: /summary/facets/temporal/unresolved == 0
#include <stdlib.h>

struct pair { int *a; int *b; };

void cleanup(struct pair *s) {
  free(s->a);
  s->a = NULL;
  free(s->b);
  s->b = NULL;
}
