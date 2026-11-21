// RFC 0051 §4.4 in C: a call through a declaration without a prototype is
// an error too, beside C's own errors for an implicit declaration.
// RUN: %weavec_cc -std=c17 -fsyntax-only -fweavec-safe -ferror-limit=0 -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
int old();

int calls(void) { return old(1, 2); } // expected-error {{passing arguments to 'old' without a prototype is deprecated}}

int sign(int x) {
  if (x > 0)
    return 1;
} // expected-error {{non-void function does not return a value in all control paths}}
