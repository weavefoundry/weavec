// RFC 0035 §2.5: library calls are guarded over the bytes their row says:
// strcpy through the runtime's checked version, memcpy's length, a %s
// argument's terminator; snprintf only over what it writes; gets byte by
// byte.
// RUN: %weavec_cc -O2 %s -o %t
// RUN: %t ok | FileCheck %s --check-prefix=OK
// RUN: not --crash %t strcpy 2>&1 | FileCheck %s --check-prefix=STRCPY
// RUN: not --crash %t memcpy 2>&1 | FileCheck %s --check-prefix=MEMCPY
// RUN: not --crash %t printf 2>&1 | FileCheck %s --check-prefix=PRINTF
// RUN: echo 'a line of 16 bytes' | not --crash %t gets 2>&1 | FileCheck %s --check-prefix=GETS
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// C11 removed gets; glibc no longer declares it.
char *gets(char *);

int main(int argc, char **argv) {
  const char *what = argv[argc - 1];
  char *small = malloc(8);
  if (strcmp(what, "ok") == 0) {
    // A size larger than the buffer is no error when the output fits.
    snprintf(small, 64, "%s", "fits");
    // OK: fits
    puts(small);
    return 0;
  }
  if (strcmp(what, "strcpy") == 0)
    // STRCPY: weavec: heap-buffer-overflow at {{.*}}checked-calls.c:[[@LINE+1]]:{{[0-9]+}}: write of 12 bytes
    strcpy(small, "eleven char");
  if (strcmp(what, "memcpy") == 0)
    // MEMCPY: weavec: heap-buffer-overflow at {{.*}}checked-calls.c:[[@LINE+1]]:{{[0-9]+}}: write of
    memcpy(small, what, (size_t)argc * 9);
  if (strcmp(what, "gets") == 0)
    // GETS: weavec: heap-buffer-overflow at {{.*}}checked-calls.c:[[@LINE+1]]:{{[0-9]+}}: write of 1 bytes
    return gets(small) == NULL;
  if (strcmp(what, "printf") == 0) {
    memset(small, 'x', 8);
    // PRINTF: weavec: unterminated-string at {{.*}}checked-calls.c:[[@LINE+1]]:{{[0-9]+}}
    printf("%s\n", small);
  }
  return 0;
}
