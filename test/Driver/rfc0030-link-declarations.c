// RFC 0030 §13.2 step 3 (probe 38), as RFC 0035 §8 keeps it: a declaration
// that says WEAVEC_BORROWED while the definition in another unit frees the
// parameter is an `annotation-mismatch` error at the declaration in
// `weavec --whole-program`, which the program's summary line counts.
//
// RUN: not %weavec --whole-program %s %S/Inputs/rfc0030-inspect.c -- 2>&1 | FileCheck --check-prefix=LINK %s
// RUN: %weavec --whole-program -Wno-error=weavec-annotation-mismatch %s %S/Inputs/rfc0030-inspect.c -- 2>&1 | FileCheck --check-prefix=LOWERED %s
#include <stdlib.h>
#include <weavec.h>

// LINK: rfc0030-link-declarations.c:[[@LINE+2]]:6: error: 'inspect' is declared WEAVEC_BORROWED here but its definition in '{{.*}}rfc0030-inspect.c' frees 'p' [weavec::annotation-mismatch]
// LINK: rfc0030-inspect.c:4:6: note: defined here
void inspect(char *WEAVEC_BORROWED p);

int main(void) {
  char *p = malloc(8);
  if (!p)
    return 1;
  p[0] = 1;
  inspect(p);
  int r = p[0];
  free(p);
  return r;
}

// LINK: weavec: program rfc0030-link-declarations: {{.*}}; 1 error, 0 warnings
// LOWERED: warning: 'inspect' is declared WEAVEC_BORROWED here
// LOWERED: weavec: program rfc0030-link-declarations: {{.*}}; 0 errors, 1 warning
