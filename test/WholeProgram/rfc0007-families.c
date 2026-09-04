// RFC 0007 across units (RFC 0005): release families travel in the program
// database, so a `FILE` handed out by one unit is checked against `free` in
// another, a wrapper of `free` defined elsewhere carries family `free`, and
// a resource handed out by another unit that is never released is a leak.
// A callee summarised from its body that records no effect on a pointer
// parameter is trusted not to retain it (RFC 0007, *Assumptions*).
//
// RUN: not %weavec --whole-program %s %S/Inputs/handle.c -- -I%S/Inputs 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --dump-analysis %s %S/Inputs/handle.c -- -I%S/Inputs 2>&1 | FileCheck --check-prefix=DUMP %s
#include <stdio.h>
#include <stdlib.h>
#include "handle.h"

// DUMP: function 'log_close': param 0: freed(fclose); stores{} returns{}
// DUMP: function 'log_open': param 0 *: read; stores{} returns{fresh(fclose), null} requires{param 0}
// DUMP: function 'xfree': param 0: freed(free); stores{} returns{}

void wrong_family(const char *path) {
  FILE *f = log_open(path);
  if (!f)
    return;
  // CHECK: rfc0007-families.c:[[@LINE+1]]:3: error: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  free(f);
}

void wrong_wrapper(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  // CHECK: rfc0007-families.c:[[@LINE+1]]:3: error: 'f' is released with 'free' but must be released with 'fclose' [weavec::mismatched-release]
  xfree(f);
}

int leaked(const char *path) {
  FILE *f = log_open(path);
  // `log_peek` reads `f` and keeps nothing: passing it is not an escape.
  // CHECK: rfc0007-families.c:[[@LINE+1]]:3: warning: 'f' is leaked [weavec::leak]
  return log_peek(f);
}

// Clean: the matching releaser from the other unit, and `xfree` on a `free`
// family resource.
void fine(const char *path) {
  FILE *f = log_open(path);
  log_close(f);
  char *s = malloc(8);
  xfree(s);
}

// CHECK: 1 warning and 2 errors generated.
