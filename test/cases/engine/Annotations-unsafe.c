// Engine pin converted from test/Annotations/unsafe.c; markers are the v0.10.0 golden diagnostics.
// WEAVEC_UNSAFE opts a function or a block out of checking.
#include "Inputs/prelude.h"
#include <weavec.h>

WEAVEC_UNSAFE void whole_function(int *p) {
  free(p);
  free(p); // not reported
}

void unsafe_block(int *p) {
  free(p);
  WEAVEC_UNSAFE {
    use(p); // not reported
  }
  use(p); // BUG: use-after-free
}
