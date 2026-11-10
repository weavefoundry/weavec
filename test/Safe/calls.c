// RFC 0041 §3, rules call-args and variadic: a bounded parameter takes a
// pointer with bounds; a variadic call is to a function the library table
// describes, and a safe function reads no variadic arguments.
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

int total(const int *WEAVEC_COUNTED_BY(n) v, size_t n);
int logf_(const char *format, ...);

WEAVEC_SAFE int args(const int *one, const int *WEAVEC_COUNTED_BY(n) v,
                     size_t n) {
  int a[4] = {1, 2, 3, 4};
  return total(a, 4) + total(v, n) + total(0, 0) +
         total(one, 1);           // expected-error {{the argument for 'v' of 'total' must have bounds, and this pointer points to one object [weavec-safe:call-args]}}
}

WEAVEC_SAFE void variadic(int x) {
  printf("%d\n", x);              // the library table describes printf
  logf_("%d", x);                 // expected-error {{'logf_' is variadic and the library table does not describe it [weavec-safe:variadic]}}
}

WEAVEC_SAFE int own(int n, ...) {
  va_list list;
  va_start(list, n);              // expected-error {{a safe function cannot read its variadic arguments [weavec-safe:variadic]}}
  va_end(list);
  return n;
}

WEAVEC_SAFE int listed(int n, va_list list) {
  int x = va_arg(list, int);      // no pointer in the type
  char *s = va_arg(list, char *); // expected-error {{a variadic argument read as a type that holds pointers could forge a pointer [weavec-safe:variadic]}}
  return x + n + (s != 0);
}

int has(const char *WEAVEC_NULL_TERMINATED s, char c);
int maybe(const char *WEAVEC_NULL_TERMINATED_OR_NULL s, char c);
WEAVEC_SAFE int nulls(void) {
  return maybe(NULL, 'x') +
         has(NULL, 'x');          // expected-error {{null passed for 's' of 'has', which points to a string [weavec-safe:call-args]}}
}
