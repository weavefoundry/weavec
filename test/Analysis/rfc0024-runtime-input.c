// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bad.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0024: input establishes the returned prefix, never the unused tail.
// CLEAN-NOT: error:
// BAD: error: cannot establish checked safety: read interval must be initialized [weavec::checking-incomplete]

//--- good.c
#include <unistd.h>
int main(void) {
  char b[8];
  ssize_t n = read(0, b, sizeof b);
  if (n <= 0) return 0;
  return b[0];
}

//--- bad.c
#include <unistd.h>
int main(void) {
  char b[8];
  ssize_t n = read(0, b, 1);
  if (n <= 0) return 0;
  return b[7];
}
