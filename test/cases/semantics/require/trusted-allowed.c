// RFC 0030 §6.3: trusted facets are allowed at every require level.
// STAGE: S3
// Trust is explicit and listed, so under -fweavec-require=proven the trusted(unsafe)
// spatial and null facets inside the region are no errors, and the function's other facets
// are proven.
// FLAGS: -fweavec-require=proven
// CLEAN
#include <weavec.h>

int peek(const int *p) {
  int v;
  WEAVEC_UNSAFE { v = p[0]; } // TRUSTED: spatial:unsafe // TRUSTED: null:unsafe
  return v;
}
