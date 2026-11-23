// RFC 0054 §3.3: a tagged union's member read while the tag says another
// stops, in both builds, before the bytes are read as a pointer.
// FLAGS: -O2 -fweavec-safe
#include <stdio.h>
#include <weavec.h>

enum kind { KIND_NUMBER, KIND_TEXT };
struct value {
  enum kind kind;
  union {
    long number WEAVEC_WHEN(KIND_NUMBER);
    const char *WEAVEC_NULL_TERMINATED text WEAVEC_WHEN(KIND_TEXT);
  } as WEAVEC_TAGGED_BY(kind);
};

__attribute__((noinline)) char first(const struct value *v) {
  return v->as.text[0]; // TRAP: union-member
}

int main(void) {
  struct value v = {.kind = KIND_NUMBER, .as.number = 0x4141414141};
  printf("%c\n", first(&v));
  return 0;
}
