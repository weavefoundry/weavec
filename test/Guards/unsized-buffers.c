// RFC 0035 §2.5: a call whose caller gives a buffer without its size
// (realpath's resolved path, gcvt's digits) writes what only the call
// computes; its wrapper writes into a buffer of the runtime's, checks what
// was written against the caller's buffer, and copies it. A buffer large
// enough gets the library's own result.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t fits | FileCheck %s --check-prefix=FITS
// RUN: not --crash %t realpath 2>&1 | FileCheck %s --check-prefix=PATH
// RUN: not --crash %t gcvt 2>&1 | FileCheck %s --check-prefix=DIGITS
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  if (strcmp(argv[1], "fits") == 0) {
    char path[PATH_MAX];
    char digits[32];
    // FITS: path /
    printf("path %s\n", realpath("/", path));
    // FITS: digits 3.14159
    printf("digits %s\n", gcvt(3.14159265, 6, digits));
    // FITS: allocated /
    char *allocated = realpath("/", NULL);
    printf("allocated %s\n", allocated);
    free(allocated);
    return 0;
  }
  char *small = malloc(8);
  if (small == NULL)
    return 1;
  if (strcmp(argv[1], "realpath") == 0) {
    // PATH: weavec: heap-buffer-overflow at {{.*}}unsized-buffers.c:[[@LINE+1]]:{{[0-9]+}}: write of 20 bytes
    (void)realpath("/usr/share/man/man1", small);
  } else {
    // DIGITS: weavec: heap-buffer-overflow at {{.*}}unsized-buffers.c:[[@LINE+1]]:{{[0-9]+}}: write of 23 bytes
    (void)gcvt(-3.14159265358979e-100, 15, small);
  }
  free(small);
  return 0;
}
