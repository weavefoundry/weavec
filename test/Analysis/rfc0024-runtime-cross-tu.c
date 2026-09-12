// RUN: split-file %s %t
// RUN: %weavec --whole-program --checked-function=main %t/main.c %t/output.c -- 2>&1 | FileCheck %s --allow-empty
// RFC 0024: format/pack requirements and output predicates cross source units.
// CHECK-NOT: error:

//--- output.c
#include <stdarg.h>
#include <stdio.h>
int output(char *b, size_t n, const char *format, ...) {
  va_list a;
  va_start(a, format);
  int result = vsnprintf(b, n, format, a);
  va_end(a);
  return result;
}

//--- main.c
#include <stddef.h>
#include <string.h>
int output(char *, size_t, const char *, ...);
int main(void) {
  char b[16];
  int n = output(b, sizeof b, "%s", "ok");
  if (n < 0) return 0;
  return strlen(b);
}
