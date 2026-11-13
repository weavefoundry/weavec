// RFC 0041 amendment 4: a local cursor's WEAVEC_BORROWED field carries the
// borrowed input down the call tree; reading past the input stops at the
// comparison with its count (RFC 0042 §4).
// FLAGS: -O2
#include <weavec.h>
#include <stddef.h>

typedef struct {
  const char *WEAVEC_BORROWED WEAVEC_COUNTED_BY(length) text;
  size_t length;
  size_t at;
} cursor;

WEAVEC_SAFE int next(cursor *c) {
  return c->text[c->at++]; // TRAP: pointer-out-of-bounds
}

WEAVEC_SAFE int digits(const char *WEAVEC_COUNTED_BY(n) text, size_t n) {
  cursor c = {text, n, 0};
  int count = 0;
  while (next(&c) != 0) // runs past the input: no NUL in it
    count++;
  return count;
}

int main(void) {
  static const char text[4] = {'1', '2', '3', '4'};
  return digits(text, 4);
}
