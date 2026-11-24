// RFC 0054 §3: a union that says its tag is used under a comparison of the
// tag; the uses that would let a pointer into a member outlive that check
// are union-pointer, and the declaration's mistakes bad-annotation.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <stdlib.h>
#include <weavec.h>

enum kind { KIND_NUMBER, KIND_TEXT, KIND_NAME, KIND_LIST, KIND_SMALL };
struct list { int n; };
struct value {
  enum kind kind;
  union payload {
    double number WEAVEC_WHEN(KIND_NUMBER);
    const char *text WEAVEC_WHEN(KIND_TEXT, KIND_NAME);
    struct list *WEAVEC_OWNED items WEAVEC_WHEN(KIND_LIST);
    char small[8] WEAVEC_WHEN(KIND_SMALL);
    long spare;
  } as WEAVEC_TAGGED_BY(kind);
};

// Reads and writes of a member, and initializers, are accepted.
char first(const struct value *v) { return v->kind == KIND_TEXT ? v->as.text[0] : 0; }
double number(struct value v) { return v.as.number; }
void set(struct value *v, const char *WEAVEC_ESCAPES s) {
  v->kind = KIND_TEXT;
  v->as.text = s;
}
struct value make(double d) {
  struct value v = {.kind = KIND_NUMBER, .as.number = d};
  return v;
}
void copy(struct value *to, const struct value *from) { *to = *from; }
// A pointer to `const` of a member that holds no pointer.
double peek(const struct value *v) {
  const double *d = &v->as.number;
  return *d;
}

void touch(const char **p);
void reslot(char *p);
void slot(struct value *v) {
  touch(&v->as.text); // expected-error {{the address of 'text' would outlive the check of 'kind' [weavec-safe:union-pointer]}} expected-error {{the address of a pointer field cannot leave a safe function [weavec-safe:field-address]}}
}
void bytes(struct value *v) {
  reslot(v->as.small); // expected-error {{the address of 'small' would outlive the check of 'kind' [weavec-safe:union-pointer]}}
}
long apart(struct value *v) {
  long n = 0;
  union payload u = v->as; // expected-error {{'as' is used without 'kind' [weavec-safe:union-pointer]}}
  (void)u;
  return n;
}
long unspoken(const struct value *v) {
  return v->as.spare; // expected-error {{'spare' has no 'WEAVEC_WHEN': no value of 'kind' is for it [weavec-safe:union-pointer]}}
}
// A union given whole has a member no tag was checked for.
const char *forge(long bits) {
  union payload u = {.spare = bits};
  struct value v = {.kind = KIND_TEXT, .as = u}; // expected-error {{'as' is initialized as a whole union, whose member 'kind' does not say [weavec-safe:union-pointer]}}
  return v.kind == KIND_TEXT ? v.as.text : 0;
}
const char *assigned(long bits) {
  union payload u = {.spare = bits};
  struct value v = {.kind = KIND_TEXT};
  v.as = u; // expected-error {{[weavec-safe:union-pointer]}}
  return v.kind == KIND_TEXT ? v.as.text : 0;
}
void late(struct value *v, const char *WEAVEC_ESCAPES s) {
  v->as.text = s; // expected-error {{'text' is stored before 'kind' is set, and the store of 'kind' clears it [weavec-safe:union-pointer]}}
  v->kind = KIND_TEXT;
}
// §3.4: the store of the tag clears an owner the union still holds.
int leak(void) {
  struct value v = {0};
  v.kind = KIND_LIST;
  v.as.items = malloc(sizeof *v.as.items);
  v.kind = KIND_NUMBER; // expected-error {{'v.as.items' is cleared by the store of 'kind' while it may still own its block [weavec-safe:owner-leak]}}
  return v.kind;
}
int released(void) {
  struct value v = {0};
  v.kind = KIND_LIST;
  v.as.items = malloc(sizeof *v.as.items);
  free(v.as.items);
  v.kind = KIND_NUMBER;
  return v.kind;
}
// Amendment: ownership does not follow the owners of a parameter's union
// (RFC 0043 follows none in unions), so a tag stored over a caller's owner
// leaks it without an error; a leak is not a memory error.
void retag(struct value *v) { v->kind = KIND_NUMBER; }

// bad-annotation: no such field, a pointer member with no values, two
// members for one value that hold different pointers.
struct missing {
  int kind;
  union { char *p WEAVEC_WHEN(0); } u WEAVEC_TAGGED_BY(tag); // expected-error {{'WEAVEC_TAGGED_BY(tag)' names no integer field of 'missing' [weavec-safe:bad-annotation]}}
};
struct unsaid {
  int kind;
  union { char *p; long n WEAVEC_WHEN(1); } u WEAVEC_TAGGED_BY(kind); // expected-error {{'p' holds a pointer and has no 'WEAVEC_WHEN' [weavec-safe:bad-annotation]}}
};
struct shared {
  int kind;
  union { char *p WEAVEC_WHEN(1); long *q WEAVEC_WHEN(1); } u WEAVEC_TAGGED_BY(kind); // expected-error {{'p' and 'q' are both for 1 and one holds a pointer [weavec-safe:bad-annotation]}}
};
int uses(struct missing *a, struct unsaid *b, struct shared *c) {
  return (a != 0) + (b != 0) + (c != 0);
}

// A mask: the tag's other bits are not the union's.
struct masked {
  unsigned char tt;
  union { long i WEAVEC_WHEN(3); double d WEAVEC_WHEN(19); } value WEAVEC_TAGGED_BY(tt, 0x1f);
};
long integer(const struct masked *m) { return m->value.i; }
