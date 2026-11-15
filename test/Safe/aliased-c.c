// RFC 0047 §5.1: WEAVEC_ALIASED is Safe C++'s.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>

struct WEAVEC_ALIASED env { int fd; }; // expected-error {{WEAVEC_ALIASED is Safe C++'s, for a class changed through pointers other objects keep; C has none [weavec-safe:bad-annotation]}}
int fd(const struct env *e) { return e->fd; }
