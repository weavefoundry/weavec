// RFC 0002: pointer copies alias, so a free through one name is a free of
// every name, and the note says which name did it.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
#include "../Inputs/prelude.h"

struct node {
  struct node *next;
  int *data;
};

void through_copy(void) {
  char *p = malloc(8);
  char *q = p;
  free(q);
  // CHECK: rfc0002-aliases.c:[[@LINE+1]]:3: error: use of 'p' after it was freed [weavec::use-after-free]
  p[0] = 0;
  // CHECK: rfc0002-aliases.c:[[@LINE-3]]:3: note: freed here (through 'q')
}

void through_conditional(int c) {
  char *p = malloc(4);
  char *q = malloc(4);
  char *r = c ? p : q;
  free(r);
  // CHECK: rfc0002-aliases.c:[[@LINE+1]]:7: error: use of 'p' after it was freed [weavec::use-after-free]
  use(p);
  // CHECK: rfc0002-aliases.c:[[@LINE+1]]:3: error: 'q' is freed twice [weavec::double-free]
  free(q);
  // CHECK: rfc0002-aliases.c:[[@LINE-5]]:3: note: previously freed here (through 'r')
}

void fields_follow_the_alias(struct node *n) {
  struct node *m = n;
  free(m->data);
  // CHECK: rfc0002-aliases.c:[[@LINE+1]]:7: error: use of 'n->data' after it was freed [weavec::use-after-free]
  use(n->data);
  // CHECK: rfc0002-aliases.c:[[@LINE-3]]:3: note: freed here (through 'm->data')
}

void copy_after_the_fact(struct node *p) {
  free(p->data);
  struct node *q = p;
  // CHECK: rfc0002-aliases.c:[[@LINE+1]]:7: error: use of 'q->data' after it was freed [weavec::use-after-free]
  use(q->data);
}

void freeing_kills_aliases(void) {
  struct node *c = malloc(sizeof *c);
  struct node *d = c;
  free(c);
  // CHECK: rfc0002-aliases.c:[[@LINE+1]]:7: error: use of 'd' after it was freed [weavec::use-after-free]
  use(d);
}

// Reassigning an alias separates it again.
void reassigned_alias(void) {
  char *p = malloc(4);
  char *q = p;
  q = malloc(4);
  free(q);
  use(p);
  free(p);
}

// The alias relation is not transitive across joins, so walking and
// freeing a list is clean (the RFC's motivating false positive).
void free_list(struct node *head) {
  while (head) {
    struct node *next = head->next;
    free(head);
    head = next;
  }
}

void unlink_nth(struct node *head, int n) {
  struct node *cur = head;
  struct node *prev = NULL;
  while (cur) {
    if (n-- > 0) {
      prev = cur;
      cur = cur->next;
      continue;
    }
    struct node *victim = cur;
    cur = cur->next;
    if (prev)
      prev->next = cur;
    free(victim);
  }
}

// CHECK: 6 errors generated.
