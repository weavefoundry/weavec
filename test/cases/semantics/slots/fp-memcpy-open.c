// RFC 0030 §8 and §9.3: through an open slot, memcpy's spatial requirements are unresolved(callback).
// STAGE: S7
// 'copy_hook' has external linkage, so other units may store any function into it: the slot
// is open. Its known target memcpy decides temporal facts only, and the row's spatial
// requirements are unresolved(callback). The call's temporal facet is not proven
// (trusted(extern-contract) in the RFC's text; the unresolved questions allow
// unresolved(callback) instead).
#include <stddef.h>
#include <string.h>

void *(*copy_hook)(void *, const void *, size_t) = memcpy;

void put(char *dst, const char *src, size_t n) {
  copy_hook(dst, src, n); // UNRESOLVED: spatial:callback // NOT-PROVEN: temporal
}
