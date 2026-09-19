// Engine pin converted from test/Analysis/rfc0002-aliases.c; markers are the v0.10.0 golden diagnostics.
// RFC 0002: pointer copies alias, so a free through one name is a free of
// every name, and the note says which name did it.
#include "Inputs/prelude.h"

struct node {
  struct node *next;
  int *data;
};

void through_copy(void) {
  char *p = malloc(8);
  char *q = p;
  free(q);
  p[0] = 0; // BUG: use-after-free
}

void through_conditional(int c) {
  char *p = malloc(4);
  char *q = malloc(4);
  char *r = c ? p : q;
  free(r);
  use(p); // BUG: use-after-free
  free(q); // BUG: double-free
}

void fields_follow_the_alias(struct node *n) {
  struct node *m = n;
  free(m->data);
  use(n->data); // BUG: use-after-free
}

void copy_after_the_fact(struct node *p) {
  free(p->data);
  struct node *q = p;
  use(q->data); // BUG: use-after-free
}

void freeing_kills_aliases(void) {
  struct node *c = malloc(sizeof *c);
  struct node *d = c;
  free(c);
  use(d); // BUG: use-after-free
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
