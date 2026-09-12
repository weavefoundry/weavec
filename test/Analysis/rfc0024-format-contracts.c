// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bad.c -- -Wno-format 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0024: truncation writes its final NUL; promoted argument types are checked.
// CLEAN-NOT: error:
// BAD: error: cannot establish checked safety: format argument must have the required promoted type [weavec::checking-incomplete]

//--- good.c
#include <stdio.h>
int main(void) {
  char b[4];
  int n = snprintf(b, sizeof b, "abcdef");
  if (n < 0) return 0;
  return b[3];
}

//--- bad.c
#include <stdio.h>
int main(void) {
  char b[16];
  return snprintf(b, sizeof b, "%s", 7);
}
