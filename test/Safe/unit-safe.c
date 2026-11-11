// RFC 0041 §10: -fweavec-safe makes every function the unit defines safe,
// but WEAVEC_GUARDED and WEAVEC_UNSAFE ones; without it only WEAVEC_SAFE.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// RUN: %weavec_cc -fsyntax-only %s
// RUN: %weavec_cc -fsyntax-only -fweavec-checks=none -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

int checked(const int *p) {
  return p[1];                    // expected-error {{[weavec-safe:index-single]}}
}

WEAVEC_GUARDED int guarded(const int *p) { return p[1]; }

WEAVEC_UNSAFE int unsafe(const int *p) { return p[1]; }

int library(const char *WEAVEC_NULL_TERMINATED s) {
  char *copy = malloc(strlen(s) + 1);
  strcpy(copy, s);
  int c = copy[0];
  free(copy);
  return c;
}
