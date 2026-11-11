// RFC 0041 §3, rule index-single: a pointer that points to one object is
// dereferenced, not indexed or offset; a bound, an array, an allocation or
// a local computed from one may be.
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stddef.h>
#include <stdlib.h>

struct buf { size_t n; int *WEAVEC_COUNTED_BY(n) data; int *one; };

WEAVEC_SAFE int single(const int *p) {
  int a = p[0] + *p;              // index 0 is *p
  return a + p[1];                // expected-error {{'p' points to one object and cannot be indexed or offset [weavec-safe:index-single]}}
}

WEAVEC_SAFE int offset(int *p) {
  p++;                            // expected-error {{[weavec-safe:index-single]}}
  return *(p + 2);                // expected-error {{[weavec-safe:index-single]}}
}

WEAVEC_SAFE int counted(const int *WEAVEC_COUNTED_BY(n) v, size_t n) {
  const int *end = v + n;
  int s = 0;
  for (const int *p = v; p < end; p++)
    s += *p;
  return s + v[n - 1];
}

WEAVEC_SAFE int ended(const char *WEAVEC_ENDED_BY(e) b, const char *e) {
  int s = 0;
  while (b < e)
    s += *b++;
  return s;
}

WEAVEC_SAFE size_t string(const char *WEAVEC_NULL_TERMINATED s) {
  size_t i = 0;
  while (s[i])
    i++;
  return i;
}

WEAVEC_SAFE int fields(struct buf *b, size_t i) {
  return b->data[i] + b->one[i];  // expected-error {{'b->one' points to one object}}
}

WEAVEC_SAFE int locals(size_t n) {
  int a[8] = {0};
  int *p = a;
  int *q = malloc(n * sizeof *q);
  int r = p[3] + q[n - 1] + "abc"[2];
  free(q);
  return r;
}

WEAVEC_SAFE int mixed(int *one, int *WEAVEC_COUNTED_BY(4) four, int which) {
  int *p = which ? one : four;
  return p[1];                    // expected-error {{'p' points to one object}}
}
