// RFC 0030 §6.2: an assumption the engine refutes is a contradicted-assumption error.
// STAGE: S3
// 'len' is 0 on every path to the site, so 'len > 0' is refuted: "assumption 'len > 0' is
// false here", with a note where 'len' got its value. v0.10.0 trusted it (probe c03).
#include <stddef.h>
#include <weavec.h>

size_t last(const char *s) {
  size_t len = 0;
  (void)s;
  WEAVEC_ASSUME(len > 0); // BUG: contradicted-assumption definite
  return len - 1;
}
