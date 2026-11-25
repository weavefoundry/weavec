// RFC 0054 §4: E9 writes a union's tag from the tests its reads are under
// (likely), and the tag's store moves before its member's (certain).
// RUN: rm -rf %t && mkdir -p %t && cp %s %t/value.c
// RUN: not %weavec_cc -fsyntax-only -fweavec-safe %t/value.c 2>&1 | FileCheck %s --check-prefix=NOTE
// RUN: %weavec_cc --weavec-fix -- %weavec_cc -fweavec-safe -c %t/value.c -o %t/value.o 2>&1 | FileCheck %s --check-prefix=FIX
// RUN: FileCheck %s --check-prefix=FIXED < %t/value.c
// RUN: %weavec_cc -fsyntax-only -fweavec-safe %t/value.c
#include <weavec.h>

enum kind { K_NUM, K_TEXT, K_NAME };
struct value {
  enum kind kind;
  // FIXED: double number WEAVEC_WHEN(K_NUM);
  // FIXED-NEXT: const char *text WEAVEC_WHEN(K_TEXT, K_NAME);
  // FIXED-NEXT: } as WEAVEC_TAGGED_BY(kind);
  union {
    double number;
    const char *text;
  } as;
};
double num(const struct value *v) {
  if (v->kind == K_NUM)
    return v->as.number;
  return 0;
}
char first(const struct value *v) {
  switch (v->kind) {
  case K_TEXT:
  case K_NAME:
    return v->as.text[0];
  default:
    return 0;
  }
}
// NOTE: error: reading union member 'text' may reinterpret another member's bytes as a pointer [weavec-safe:union-pointer]
// NOTE: note: fix (likely): declare 'WEAVEC_TAGGED_BY(kind)' on 'as' and 'WEAVEC_WHEN' on its members, by the tests of 'kind' its reads are under
// FIXED: void set(struct value *v, const char *WEAVEC_ESCAPES s) {
// FIXED-NEXT: v->kind = K_TEXT;
// FIXED-NEXT: v->as.text = s;
void set(struct value *v, const char *WEAVEC_ESCAPES s) {
  v->as.text = s;
  v->kind = K_TEXT;
}
// FIX: 0 errors after
