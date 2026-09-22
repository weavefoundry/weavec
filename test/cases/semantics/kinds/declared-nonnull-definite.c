// RFC 0030 §7.2 (WEAVEC_NONNULL on a parameter): a definitely-null argument is still an error.
// STAGE: S3
// A null pointer constant passed for a WEAVEC_NONNULL parameter is null on every path and
// not from an allocator, so it is a definite null-dereference error at the call (§3.2).
#include <stddef.h>
#include <weavec.h>

int get(const int *WEAVEC_NONNULL p) { return *p; }

int use(void) { return get(NULL); } // BUG: null-dereference definite
