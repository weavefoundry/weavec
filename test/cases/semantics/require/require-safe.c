// RFC 0030 §6.3: WEAVEC_REQUIRE_SAFE holds one function's sites to -fweavec-require=checked.
// STAGE: S3
// Without flags the level is none: 'lax' only records its unresolved facets. 'strict' is
// annotated WEAVEC_REQUIRE_SAFE, so each of its unresolved facets is an
// unresolved-operation error, whatever the command line says.
#include "../Inputs/rfc0030.h"

char *get_buffer(void);

int lax(void) {
  char *b = get_buffer(); // UNRESOLVED: temporal:unknown-callee
  return b[1000]; // UNRESOLVED: spatial:unknown-extent
}

WEAVEC_REQUIRE_SAFE int strict(void) {
  char *b = get_buffer(); // BUG: unresolved-operation definite
  return b[1000]; // BUG: unresolved-operation definite
}
