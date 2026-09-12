// RUN: split-file %s %t
// RUN: %weavec --checked-function=main %t/good.c -- 2>&1 | FileCheck %s --check-prefix=CLEAN --allow-empty
// RUN: not %weavec --checked-function=main %t/bad.c -- 2>&1 | FileCheck %s --check-prefix=BAD
// RFC 0024: a copied cursor is independent; a consumed cursor cannot be traversed.
// CLEAN-NOT: error:
// BAD: error: cannot establish checked safety: variadic traversal requires an active unconsumed argument list [weavec::checking-incomplete]

//--- good.c
#include <stdarg.h>
#include <stdio.h>
static void output(const char *format, ...) {
  va_list a, b;
  va_start(a, format);
  va_copy(b, a);
  vprintf(format, a);
  vprintf(format, b);
  va_end(a);
  va_end(b);
}
int main(void) { output("%s", "ok"); return 0; }

//--- bad.c
#include <stdarg.h>
#include <stdio.h>
static void output(const char *format, ...) {
  va_list a;
  va_start(a, format);
  vprintf(format, a);
  vprintf(format, a);
  va_end(a);
}
int main(void) { output("%s", "ok"); return 0; }
