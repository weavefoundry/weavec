// RFC 0033 §2: a pointer declared WEAVEC_RAW is still raw, and dereferencing it outside an
// unsafe region is still an error.
// STAGE: S1
// TOOL
#include <weavec.h>
int read_raw(int *WEAVEC_RAW p) {
  return *p; // BUG: unsafe-operation definite
}
