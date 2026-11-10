// RFC 0041 §12: safe functions are C only.
// RUN: %weavec_cxx -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>

WEAVEC_SAFE int f(int x) { return x; } // expected-error {{safe functions are C only (RFC 0041 §12) [weavec-safe:cxx]}}
