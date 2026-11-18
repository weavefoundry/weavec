// RFC 0049 §3, §3.5: Safe C's errors carry fixes as Safe C++'s do: a bound
// from a library call's count (E1), from a loop's comparison (E2), a
// string from a byte tested against zero (E4); a parameter stored where it
// outlives the call is kept (C1, certain); a returned parameter is
// WEAVEC_LIFETIMEBOUND when every return gives it (E7).
// RUN: not %weavec_cc -fsyntax-only -fweavec-safe -w -ferror-limit=0 -fdiagnostics-parseable-fixits %s 2>&1 | FileCheck %s
#include <stddef.h>
#include <string.h>
#include <weavec.h>

// A named count wins over constant indices (E5).
char copy(char *WEAVEC_COUNTED_BY(n) to, const char *from, size_t n) {
  memcpy(to, from, n);
  return from[1];
}
// CHECK: error: 'from' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: fix (likely): declare it 'const char *WEAVEC_COUNTED_BY(n) from'; 'n' is its count in the call to 'memcpy' at line 13
// CHECK: fix-it:"{{.*}}fix-bounds.c":{12:54-12:54}:"WEAVEC_COUNTED_BY(n) "

int sum(const int *values, size_t count) {
  int total = 0;
  for (size_t i = 0; i < count; ++i)
    total += values[i];
  return total;
}
// CHECK: error: 'values' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: fix (likely): declare it 'const int *WEAVEC_COUNTED_BY(count) values'; 'count' bounds its index at line 22

size_t span(const char *s) {
  size_t n = 0;
  while (s[n])
    ++n;
  return n;
}
// CHECK: error: 's' points to one object and cannot be indexed or offset [weavec-safe:index-single]
// CHECK: note: fix (likely): declare it 'const char *WEAVEC_NULL_TERMINATED s'; a byte of it is tested against zero at line 31

struct holder {
  int *kept;
};
void keep(struct holder *h, int *value) { h->kept = value; }
// CHECK: error: 'value' is borrowed for this call and cannot be stored where it outlives the call [weavec-safe:escape]
// CHECK: note: fix (certain): declare it 'int *WEAVEC_ESCAPES value'

const char *same(const char *p) { return p; }
// CHECK: error: 'p' is borrowed for this call and cannot be returned [weavec-safe:escape]
// CHECK: note: fix (likely): declare it 'const char *p WEAVEC_LIFETIMEBOUND'; every return of 'same' gives 'p'
// CHECK: fix-it:"{{.*}}fix-bounds.c":{45:31-45:31}:" WEAVEC_LIFETIMEBOUND"
// CHECK: note: 5 of 5 errors carry a fix; 'weavec-cc --weavec-fix -- <this command>' applies them
