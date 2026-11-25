// RFC 0054 §3.3: the twin of tagged-wrong-member.c: the read is under the
// tag it is for, and a new tag clears the union before its member is set.
// CLEAN
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
  return v->kind == KIND_TEXT ? v->as.text[0] : '?';
}

int main(void) {
  struct value v = {.kind = KIND_NUMBER, .as.number = 0x4141414141};
  char a = first(&v);
  v.kind = KIND_TEXT;
  v.as.text = "ok";
  printf("%c%c\n", a, first(&v));
  return 0;
}
