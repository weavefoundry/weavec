// RFC 0030 §7.2 (WEAVEC_NULLABLE on a result): a declared nullable result is never proven non-null.
// STAGE: S6
// 'lookup' returns a non-null pointer on every path, but its declaration says the result may
// be null, and WEAVEC_* annotations take precedence over what the body shows. The
// dereference of the result is therefore not proven: its null facet is checked.
#include <weavec.h>

static int table[4] = {1, 2, 3, 4};

int *WEAVEC_NULLABLE lookup(int k) { return &table[k & 3]; }

int first(void) { return *lookup(0); } // NOT-PROVEN: null
