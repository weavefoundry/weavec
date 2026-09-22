// Engine pin converted from test/Annotations/unsafe.c; markers are the v0.10.0 golden diagnostics,
// plus the two RFC 0030 §6.1 adds: no diagnostic is dropped for being inside an unsafe region
// (v0.10.0 reported nothing inside one).
// WEAVEC_UNSAFE no longer opts a function or a block out of checking (RFC 0030 §6.1).
#include "Inputs/prelude.h"
#include <weavec.h>

WEAVEC_UNSAFE void whole_function(int *p) {
  free(p);
  free(p); // BUG: double-free definite
}

void unsafe_block(int *p) {
  free(p);
  WEAVEC_UNSAFE {
    use(p); // BUG: use-after-free definite
  }
  use(p); // BUG: use-after-free
}
