// RFC 0030 §7.3, slot kinds: every pointer field and pointer variable with
// static storage starts `single` and is demoted to `unknown`, as a greatest
// fixpoint, by a store whose value is not Single-valid, and by the stores
// the syntax does not show: through `*q` into an address-taken slot, a slot
// address passed to a callee that may store through it, a byte-wise write,
// a store to another member of a union. Each demoted slot lists why.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s

typedef unsigned long size_t;
void *malloc(size_t);
void *memcpy(void *, const void *, size_t);
void *memset(void *, int, size_t);
void opaque(char **where);

struct node {
  // CHECK: field node.next: single nullable [inferred, lower-bound]
  struct node *next;
  // CHECK: field node.name: single nullable [inferred, lower-bound]
  const char *name;
  // CHECK: field node.follow: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+22]]:3: a load from 'node.cursor', which is unknown
  struct node *follow;
  // CHECK: field node.cursor: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+18]]:3: pointer arithmetic
  struct node *cursor;
  // CHECK: field node.tiny: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+17]]:3: a value with 4 bytes, fewer than the 48 of 'struct node'
  struct node *tiny;
  // CHECK: field node.sized: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+15]]:3: an allocation by 'malloc' of a size that is not constant
  struct node *sized;
};

// CHECK: variable last: single nullable [inferred, lower-bound]
static struct node *last;

void link(struct node *n, size_t count, const char *label) {
  struct node *fresh = malloc(sizeof *fresh);
  n->next = fresh;
  n->name = label;
  last = n;
  n->cursor = n + 1;
  n->follow = n->cursor;
  n->tiny = malloc(4);
  n->sized = malloc(count);
  n->name = "literal";
  n->next = 0;
}

// Stores through a pointer reach every address-taken slot of its type.
struct holder {
  // CHECK: field holder.slot: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+10]]:3: pointer arithmetic
  char *slot;
  // CHECK: field holder.passed: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+7]]:3: pointer arithmetic
  // CHECK-NEXT: demoted at [[@LINE+7]]:3: its address is passed to 'opaque'
  char *passed;
};

void through(struct holder *h, char *buffer) {
  char **where = &h->slot;
  *where = buffer + 1;
  opaque(&h->passed);
}

// A byte-wise write demotes the object's pointer fields; a zero fill does
// not.
struct copied {
  // CHECK: field copied.data: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+9]]:3: a byte-wise write by 'memcpy'
  int *data;
};
struct zeroed {
  // CHECK: field zeroed.data: single nullable [inferred, lower-bound]
  int *data;
};

void bytes(struct copied *c, struct zeroed *z, const char *raw) {
  memcpy(c, raw, sizeof *c);
  memset(z, 0, sizeof *z);
}

// A store to another member of a union rewrites the pointer's bytes.
union value {
  // CHECK: field value.p: unknown nullable [inferred]
  // CHECK-NEXT: demoted at [[@LINE+5]]:28: a store to 'value.n', another member of the union
  int *p;
  long n;
};

void pun(union value *v) { v->n = 1; }
