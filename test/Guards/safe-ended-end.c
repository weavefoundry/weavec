// RFC 0047 amendment: the end a WEAVEC_ENDED_BY names points past the
// elements, not to an object, so an end one past an array is not checked
// as one (`bounds-conversion` stopped it before).
// RUN: %weavec_cc -O2 -g -fweavec-safe %s -o %t
// RUN: %t | FileCheck %s
#include <stdio.h>
#include <weavec.h>

unsigned decode(const char *WEAVEC_ENDED_BY(limit) p, const char *limit) {
  unsigned r = 0;
  while (p < limit)
    r = r * 10 + (unsigned)(*p++ - '0');
  return r;
}
WEAVEC_GUARDED unsigned via(const char *p, unsigned n) { return decode(p, p + n); }
int main(void) {
  char text[6] = {'1', '2', '3', '4', '5', '6'};
  // CHECK: 123456 123456
  printf("%u %u\n", decode(text, text + 6), via(text, 6));
  return 0;
}
