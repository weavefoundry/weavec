// RFC 0054 §3.2 (amendments): a tagged union's tag and members are used
// only in the forms its checks see, and its initializers agree with its
// tag; a reallocation and a global's initializer convert no type with
// pointers. Each case is one of G6's first round.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <stdint.h>
#include <stdlib.h>
#include <weavec.h>

enum kind { KIND_NUMBER, KIND_TEXT };
struct value {
  enum kind kind;
  union {
    long number WEAVEC_WHEN(KIND_NUMBER);
    const char *WEAVEC_NULL_TERMINATED text WEAVEC_WHEN(KIND_TEXT);
    long numbers[2] WEAVEC_WHEN(KIND_NUMBER);
  } as WEAVEC_TAGGED_BY(kind);
};

char wrongInit(long n) {
  struct value v = {.kind = KIND_TEXT, .as.number = n}; // expected-error {{'number' is initialized while 'kind' is 1 [weavec-safe:union-pointer]}}
  return v.as.text[0];
}
char positional(long n) {
  struct value v = {KIND_TEXT, {n}}; // expected-error {{'number' is initialized while 'kind' is 1 [weavec-safe:union-pointer]}}
  return v.as.text[0];
}
char literal(long n) {
  struct value v;
  v = (struct value){.kind = KIND_TEXT, .as.number = n}; // expected-error {{'number' is initialized while 'kind' is 1 [weavec-safe:union-pointer]}}
  return v.as.text[0];
}
long rightInit(long n) {
  struct value v = {.kind = KIND_NUMBER, .as.number = n};
  v.as.numbers[1] = 7; // an element in place
  return v.as.number + v.as.numbers[1];
}

void setTag(enum kind *k);
void retagThrough(struct value *v) {
  enum kind *k = &v->kind; // expected-error {{'kind' is the tag of 'as': it is changed only by an assignment, which clears the union [weavec-safe:union-pointer]}}
  *k = KIND_TEXT;
  setTag(&v->kind); // expected-error {{'kind' is the tag of 'as': it is changed only by an assignment, which clears the union [weavec-safe:union-pointer]}}
}

struct twice {
  int kind;
  union { long n WEAVEC_WHEN(0); char *p WEAVEC_WHEN(1); } a WEAVEC_TAGGED_BY(kind); // expected-error {{'kind' is the tag of two unions [weavec-safe:bad-annotation]}}
  union { long n WEAVEC_WHEN(0); char *p WEAVEC_WHEN(1); } b WEAVEC_TAGGED_BY(kind); // expected-error {{'kind' is the tag of two unions [weavec-safe:bad-annotation]}}
};
int useTwice(struct twice *t) { return t->kind; }

// A long tag with a negative value.
struct wide {
  long tag;
  union { long n WEAVEC_WHEN(-1); char *p WEAVEC_WHEN(1); } u WEAVEC_TAGGED_BY(tag);
};
long negative(const struct wide *w) { return w->u.n; }

struct box { char *WEAVEC_NULL_TERMINATED p; long n; };
struct box *WEAVEC_OWNED regrow(long *WEAVEC_OWNED l) {
  struct box *b = realloc(l, sizeof *b); // expected-error {{'realloc' keeps the bytes of its block: they become a 'struct box' only from a 'struct box *' [weavec-safe:pointer-cast]}} expected-error@+1 {{returning while 'l' may still own its block [weavec-safe:owner-leak]}}
  return b;
}

static long cell = 0;
static struct box *forged = (struct box *)(void *)&cell; // expected-error {{a conversion in a global's initializer is not checked [weavec-safe:pointer-cast]}}
static struct value early = {.kind = KIND_TEXT, .as.number = 1}; // expected-error {{'number' is initialized while 'kind' is 1 [weavec-safe:union-pointer]}}
char globals(void) { return forged->p[0] + early.as.text[0]; }
