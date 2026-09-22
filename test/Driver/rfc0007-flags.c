// RFC 0007, *Diagnostics*: `leak` is a warning that can be disabled or
// promoted; a definite `mismatched-release` is an error that can only be
// lowered (RFC 0030: disabling the id drops only its possible findings).
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: %weavec -Wno-weavec-leak %s -- 2>&1 | FileCheck --allow-empty --check-prefix=QUIET %s
// RUN: not %weavec -Werror=weavec-leak %s -- 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: not %weavec %s -- -DMISMATCH 2>&1 | FileCheck --check-prefix=MISMATCH %s
// RUN: %weavec -Wno-error=weavec-mismatched-release %s -- -DMISMATCH 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: not %weavec -Wno-weavec-mismatched-release %s -- -DMISMATCH 2>&1 | FileCheck --check-prefix=MISMATCH %s
#include <stdio.h>
#include <stdlib.h>

#ifdef MISMATCH
void mismatch(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  // MISMATCH: rfc0007-flags.c:[[@LINE+2]]:3: error: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  // LOWERED: rfc0007-flags.c:[[@LINE+1]]:3: warning: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  free(f);
}
#else
void leak(void) {
  char *p = malloc(8);
  if (!p)
    return;
  // DEFAULT: rfc0007-flags.c:[[@LINE+2]]:3: warning: 'p' is leaked [weavec::leak]
  // RAISED: rfc0007-flags.c:[[@LINE+1]]:3: error: 'p' is leaked [weavec::leak]
  p[0] = 0;
}
#endif

// QUIET-NOT: {{warning|error}}:
