// RFC 0030 §10.3 rule 1 and §10.6: the overlap check of a copy whose source
// is computed (`s + 1`) has no expressible second pointer. The planner marks
// the requirement unresolved(inexpressible); it must never reach the emitter
// and fail the compile with an internal error.
//
// RUN: %weavec_cc -c %s -o %t.o 2>&1 | FileCheck --allow-empty %s
// RUN: %weavec_cc -O2 -c %s -o %t.o 2>&1 | FileCheck --allow-empty %s
// CHECK-NOT: internal error

#include <string.h>

void bytes(char *d, char *s, unsigned long n) { memcpy(d, s + 1, n); }
void same(char *b, unsigned long n) { memcpy(b, b + 8, n); }
void ints(int *d, int *s, unsigned long n) { memcpy(d, s + 1, n); }
