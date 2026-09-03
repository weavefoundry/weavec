// RFC 0007, *Diagnostics*: `leak` is a warning that can be disabled or
// promoted; `mismatched-release` is an error that can only be lowered.
// RUN: %weavec %s -- 2>&1 | FileCheck --check-prefix=DEFAULT %s
// RUN: %weavec -Wno-weavec-leak %s -- 2>&1 | count 0
// RUN: not %weavec -Werror=weavec-leak %s -- 2>&1 | FileCheck --check-prefix=RAISED %s
// RUN: not %weavec %s -- -DMISMATCH 2>&1 | FileCheck --check-prefix=MISMATCH %s
// RUN: %weavec -Wno-error=weavec-mismatched-release %s -- -DMISMATCH 2>&1 | FileCheck --check-prefix=LOWERED %s
// RUN: not %weavec -Wno-weavec-mismatched-release %s -- -DMISMATCH 2>&1 | FileCheck --check-prefix=REFUSED %s
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
  // DEFAULT: rfc0007-flags.c:[[@LINE+2]]:3: warning: 'p' is leaked [weavec::leak]
  // RAISED: rfc0007-flags.c:[[@LINE+1]]:3: error: 'p' is leaked [weavec::leak]
  p[0] = 0;
}
#endif

// REFUSED: error: '-Wno-weavec-mismatched-release': 'mismatched-release' is an error and cannot be disabled; use -Wno-error=weavec-mismatched-release to make it a warning
