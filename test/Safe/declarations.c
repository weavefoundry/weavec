// RFC 0041 §2, amendment 19: every declaration of a function with bounds,
// and its safe definition, gives the same bounds; a bound's macro has one
// value in the unit; a struct's bounds may name a field of an anonymous
// struct in it, and a unit without safe functions does not check them.
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// RUN: %weavec_cc -fsyntax-only -DGUARDED %s
#include <weavec.h>
#include <stddef.h>

struct counted {
  struct { int count; };
  int items[] __attribute__((counted_by(count)));
};

#ifndef GUARDED
#define SIZE 4
#define SIZE 4
#define LATER 4

int first(const int *WEAVEC_COUNTED_BY(n) p, size_t n); // expected-note {{write the same bounds}}
WEAVEC_SAFE int first(const int *p, size_t n) { // expected-error {{this declaration of 'first' gives its pointers other bounds than an earlier one [weavec-safe:bad-annotation]}}
  (void)n;
  return *p;
}

int second(const int *WEAVEC_COUNTED_BY(n) p, size_t n);
WEAVEC_SAFE int second(const int *WEAVEC_COUNTED_BY(n) p, size_t n) {
  return n != 0 ? p[n - 1] : 0;
}

// A guarded definition need not repeat its declaration's bounds.
int third(const int *WEAVEC_COUNTED_BY(n) p, size_t n);
int third(const int *p, size_t n) { return p[n - 1]; }

WEAVEC_SAFE int fixed(const int *WEAVEC_COUNTED_BY(SIZE) p) { return p[3]; }

WEAVEC_SAFE int moved(const int *WEAVEC_COUNTED_BY(LATER) p) { // expected-error {{the macro 'LATER' in the bound is undefined or redefined in this unit}} expected-note {{give the bound as an integer literal}}
  return *p;
}
#undef LATER
#define LATER 64

WEAVEC_SAFE int last(const struct counted *c) { return c->count; }
#endif
