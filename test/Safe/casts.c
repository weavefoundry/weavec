// RFC 0041 §3, rules int-to-pointer, pointer-cast, pointer-bytes and
// union-pointer: no pointer is forged from an integer, from bytes of
// another type, or from a union member that may hold something else.
// RUN: %weavec_cc -fsyntax-only -Xclang -verify -Xclang -verify-ignore-unexpected=note %s
#include <weavec.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct node { struct node *next; int v; };
struct pair { int a, b; };
union cell { int *p; long l; };
struct tagged { int tag; union cell c; };
void opaque(void *context);

WEAVEC_SAFE int *forge(uintptr_t x) {
  return (int *)x;                // expected-error {{an integer cannot become a pointer in a safe function [weavec-safe:int-to-pointer]}}
}

WEAVEC_SAFE int *null(void) { return (int *)0; }

WEAVEC_SAFE struct node *typed(void *v, char *WEAVEC_SIZED_BY(n) b, size_t n) {
  struct pair *p = (struct pair *)b;   // no pointers: a view of bytes
  (void)p;
  struct node *fresh = malloc(sizeof *fresh);
  free(fresh);
  return (struct node *)v;        // expected-error {{converting 'void *' to 'struct node *' would read memory as a type that holds pointers [weavec-safe:pointer-cast]}} expected-error {{'v' is borrowed for this call and cannot be returned}}
}

WEAVEC_SAFE void bytes(struct node *n, struct node *m, struct pair *q) {
  memcpy(n, m, sizeof *n);        // a copy between two struct node
  memset(n, 0, sizeof *n);        // a fill with zero
  memset(n, 1, sizeof *n);        // expected-error {{writing 'struct node' as bytes could forge the pointers it holds [weavec-safe:pointer-bytes]}}
  memcpy(n, q, sizeof *q);        // expected-error {{[weavec-safe:pointer-bytes]}}
  unsigned char *b = (unsigned char *)n; // expected-error {{[weavec-safe:pointer-bytes]}}
  const unsigned char *c = (const unsigned char *)n;  // read-only
  (void)b;
  (void)c;
  opaque(n);                      // a function that is not safe keeps it
}

WEAVEC_SAFE int *unions(struct tagged *t, int *p) {
  long l = t->c.l;                // no pointer in the member read
  (void)l;
  t->c.p = 0;                     // written, not read
  return t->c.p;                  // expected-error {{reading union member 'p' may reinterpret another member's bytes as a pointer [weavec-safe:union-pointer]}}
}

typedef int (*unary)(int);
typedef long (*other)(long);
WEAVEC_SAFE other functions(unary f) {
  return (other)f;                // expected-error {{a function pointer cannot convert to another type in a safe function}}
}

WEAVEC_SAFE int present(unary f) { return f != NULL; }
