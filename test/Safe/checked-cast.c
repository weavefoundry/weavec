// RFC 0054 §2.1: the conversions a safe C function makes as checks, and
// those that stay pointer-cast.
// RUN: %weavec_cc -fsyntax-only -fweavec-safe -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <weavec.h>

struct node { struct node *next; int v; };
struct shape { int kind; };
struct circle { struct shape base; double radius; };
struct ring { struct circle outer; double inner; };
struct link { struct link *next; };
struct item { int key; struct link link; };
struct plain { long a; long b; };

// (a) `void *` to a struct.
int peek(void *context) {
  struct node *n = context;
  return n->v;
}
// (b) a struct to one that begins with it, at any depth, and back (no check).
double radius(struct shape *s) { return ((struct circle *)s)->radius; }
double inner(struct shape *s) { return ((struct ring *)s)->inner; }
int up(struct circle *c) { return ((struct shape *)c)->kind; }
// (c) container_of.
int key(struct link *l) {
  return ((struct item *)((char *)l - offsetof(struct item, link)))->key;
}
// (d) `void *` to a plain scalar.
int count(void *context) { return *(int *)context; }

// Still rejected: unrelated structs, bytes to a struct with pointers,
// pointers to pointers, function pointers.
long pun(struct node *n) {
  return ((struct plain *)n)->a; // expected-error {{converting 'struct node *' to 'struct plain *': 'struct node' holds a pointer [weavec-safe:pointer-cast]}}
}
int forged(char *WEAVEC_COUNTED_BY(16) bytes) {
  return ((struct node *)bytes)->v; // expected-error {{[weavec-safe:pointer-cast]}}
}
int slots(struct node **n) {
  return ((void **)n)[0] != 0; // expected-error {{[weavec-safe:pointer-cast]}}
}
typedef int (*binary)(int, int);
int callback(void *p) {
  return ((binary)p)(1, 2); // expected-error {{[weavec-safe:pointer-cast]}}
}

// Amendment 18: a struct with pointers is a `void *` handle to a direct or
// an indirect call, so a library function that writes through a pointer
// has no address in a safe function (it would forge the handle's pointers).
typedef void (*visit)(void *context);
void each(struct node *n, visit f) { f(n); }
typedef void *(*copier)(void *, const void *, size_t);
void copy(struct node *n, const unsigned char *WEAVEC_COUNTED_BY(16) bytes,
          copier c) {
  c(n, bytes, sizeof *n);
}
void forge(struct node *n, const unsigned char *WEAVEC_COUNTED_BY(16) bytes) {
  copy(n, bytes, memcpy); // expected-error {{'memcpy' writes bytes through a pointer it is given; called through its address, it could forge the pointers of what it writes [weavec-safe:pointer-bytes]}}
  copier c = &memmove; // expected-error {{'memmove' writes bytes through a pointer it is given; called through its address, it could forge the pointers of what it writes [weavec-safe:pointer-bytes]}}
  copy(n, bytes, c);
  memcpy(n, n, sizeof *n);
}
void release(struct node *n) { each(n, free); }
