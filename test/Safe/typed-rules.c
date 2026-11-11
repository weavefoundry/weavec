// RFC 0042 §3: the rules typed functions add to Safe C's, each with an
// accepted and a rejected form: counted-pair, string-write, string-index,
// field-address, and pointer-cast and pointer-bytes tightened.
// RUN: %weavec_cc -fsyntax-only -fweavec-typed -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
// Without -fweavec-typed the same unit is plain Safe C, and none of them
// applies.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe %s 2>&1 | FileCheck %s --allow-empty --check-prefix=SAFE
// SAFE-NOT: counted-pair
// SAFE-NOT: string-write
// SAFE-NOT: string-index
// SAFE-NOT: field-address
#include <weavec.h>
#include <stdlib.h>
#include <string.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t len, cap; };
struct node { struct node *next; int value; };
struct plain { long a, b; };
struct small { int x; };
struct big { int x, y, z; };
int use(int *p);
void edit(char *p);

// counted-pair
int grow(struct vec *v) {
  v->cap *= 2; // expected-error {{'cap' bounds 'data' and is assigned without it [weavec-safe:counted-pair]}}
  int *d = realloc(v->data, v->cap * sizeof *d);
  if (d == NULL)
    return -1;
  v->data = d;
  return 0;
}

int growPaired(struct vec *v) {
  size_t cap = v->cap ? 2 * v->cap : 8;
  int *d = realloc(v->data, cap * sizeof *d);
  if (d == NULL)
    return -1;
  v->data = d;
  v->cap = cap;
  return 0;
}

int growCountFirst(struct vec *v, size_t cap) {
  v->cap = cap; // expected-error {{'cap' and 'data' are assigned with a call between them [weavec-safe:counted-pair]}}
  v->data = calloc(cap, sizeof *v->data);
  return v->data == NULL;
}

void steps(struct vec *v) {
  v->len++;
  v->cap--;  // a decrease alone
  v->cap += 1; // checked at run time
  v->cap = 0;
}

static int four[4];
struct vec fixed = {four, 0, 4};
struct vec over = {four, 0, 8}; // expected-error {{'data' is initialized with fewer bytes than 'cap' gives it [weavec-safe:counted-pair]}}

// string-write and string-index
void trim(char *WEAVEC_NULL_TERMINATED s) {
  s[strlen(s) - 1] = '\0'; // expected-error {{store through 's', which points into a string: typed code only reads a string [weavec-safe:string-write]}}
}

void trimCounted(char *WEAVEC_COUNTED_BY(cap) s, size_t cap) {
  size_t n = strnlen(s, cap);
  if (n > 0)
    s[n - 1] = '\0';
}

void pass(char *WEAVEC_NULL_TERMINATED s) {
  edit(s); // expected-error {{'s' points into a string, and 'p' of 'edit' may write through it: typed code only reads a string [weavec-safe:string-write]}}
}

size_t count(const char *WEAVEC_NULL_TERMINATED s) {
  size_t n = 0;
  for (; *s; s++)
    n++;
  return n;
}

char at(const char *WEAVEC_NULL_TERMINATED s, size_t i) {
  return s[i]; // expected-error {{'s' points into a string and is indexed or moved by a variable or backward; a string is read at its current byte and stepped forward by constants [weavec-safe:string-index]}}
}

char measured(const char *WEAVEC_NULL_TERMINATED s, size_t i) {
  size_t n = strlen(s);
  return i < n ? s[i] : 0;
}

char back(const char *WEAVEC_NULL_TERMINATED s) {
  const char *p = s;
  p++;
  p--; // expected-error {{[weavec-safe:string-index]}}
  return s[1];
}

// field-address
struct node **link(struct node *n WEAVEC_LIFETIMEBOUND) {
  return &n->next; // expected-error {{the address of a pointer field cannot leave a typed function [weavec-safe:field-address]}}
}

void unlink(struct node **head);
void unlinkFirst(struct node *list) {
  unlink(&list->next); // expected-error {{[weavec-safe:field-address]}}
}

int last(struct node *list) {
  struct node **at = &list->next; // stays in the function
  while (*at != NULL && (*at)->next != NULL)
    at = &(*at)->next;
  return *at != NULL ? (*at)->value : list->value;
}

// pointer-cast
long pun(struct node *n) {
  return ((struct plain *)n)->a; // expected-error {{converting 'struct node *' to 'struct plain *': 'struct node' holds a pointer [weavec-safe:pointer-cast]}}
}

int fromVoid(void *context) {
  struct small *s = context; // expected-error {{converting 'void *' to 'struct small *' in a typed function: a 'void *' may hide memory that holds pointers [weavec-safe:pointer-cast]}}
  return s->x;
}

void bytesFromVoid(void *context) {
  char *b = context; // expected-error {{[weavec-safe:pointer-cast]}}
  b[0] = 0;
}

int views(struct small *s, char *WEAVEC_SIZED_BY(n) bytes, size_t n) {
  const unsigned char *read = (const unsigned char *)s; // read-only bytes
  struct big *grown = (struct big *)s;   // its accesses are compared with s's object
  struct small *header = (struct small *)bytes; // bytes hide no pointer
  struct small *made = malloc(sizeof *made);    // an allocation's result
  int r = read[0] + grown->x + header->x + (made != NULL);
  free(made);
  return r;
}

// pointer-bytes: whole objects only
void clear(struct node *n, size_t k) {
  memset(n, 0, sizeof *n);
  memset(n, 0, k * sizeof *n);
  memset(n, 0, 4); // expected-error {{writing 'struct node' as bytes could forge the pointers it holds [weavec-safe:pointer-bytes]}}
}

// counted-pair: a pair under a case label, and in a nested struct.
struct desc { int *WEAVEC_COUNTED_BY(n) p; int n; };
struct outer { struct desc d; int arr[10]; };
void labeled(struct desc *d, struct vec *o, int k) {
  switch (k) {
  case 1:
    d->n = (int)o->cap;
    d->p = o->data;
    break;
  default:
    break;
  }
}
void nested(struct outer *o) {
  o->d.p = o->arr;
  o->d.n = 10;
}

// call-args, for typed functions: a pointer to one object stored where a
// declaration bounds it, or returned as a bounded result; releasing a string
// is not a write.
struct cursor { const char *WEAVEC_NULL_TERMINATED next; };
void point(struct cursor *c, const char *WEAVEC_ESCAPES text) {
  c->next = text; // expected-error {{'text' points to one object, and field 'next' has a bound [weavec-safe:call-args]}}
}
const char *WEAVEC_NULL_TERMINATED name(const char *one WEAVEC_LIFETIMEBOUND) {
  return one; // expected-error {{'one' points to one object, and the result has a bound [weavec-safe:call-args]}}
}
void release(char *WEAVEC_NULL_TERMINATED owned) { free(owned); }
