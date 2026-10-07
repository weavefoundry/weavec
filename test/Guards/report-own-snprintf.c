// RFC 0035 §5.2: the runtime formats its reports itself, so a program that
// defines its own snprintf (an embedded printf without %ll) still gets a
// readable report.
// RUN: %weavec_cc -O0 %s -o %t
// RUN: not --crash %t 2>&1 | FileCheck %s
#include <stdarg.h>
#include <stddef.h>
#include <stdlib.h>

int snprintf(char *s, size_t n, const char *format, ...) {
  (void)format;
  if (n != 0)
    s[0] = 0;
  return 0;
}

int main(void) {
  char *p = malloc(4);
  // CHECK: weavec: heap-buffer-overflow at {{.*}}report-own-snprintf.c:[[@LINE+1]]:{{[0-9]+}}: write of 1 bytes at 0x{{[0-9a-f]+}}
  p[4] = 1;
  return 0;
}
