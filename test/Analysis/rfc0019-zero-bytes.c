// RUN: %weavec --checked-function=f %s -- -DCASE=0 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=f %s -- -DCASE=1 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %weavec --checked-function=f %s -- -DCASE=2 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %weavec --checked-function=f %s -- -DCASE=3 2>&1 | FileCheck %s --check-prefix=BAD
// RUN: not %weavec --checked-function=f %s -- -DCASE=4 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0019: a terminator witness does not assert an exact string length.
#include <string.h>
unsigned long f(char byte, int flag) {
  char bytes[4];
#if CASE != 2
  memset(bytes, byte, sizeof bytes);
#endif
#if CASE == 4
  if (flag)
#endif
    bytes[3] = 0;
#if CASE == 1
  bytes[3] = byte;
#elif CASE == 3
  char *alias = bytes;
  alias[3] = byte;
#endif
  return strlen(bytes);
}
// CLEAN-NOT: checking-incomplete
// CLEAN-NOT: checking-failed
// BAD: error: cannot establish checked safety: string input must have an initialized terminator [weavec::checking-incomplete]
