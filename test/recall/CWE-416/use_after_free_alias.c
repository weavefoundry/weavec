// CWE-416: use after free through an alias, a field pointer, and a callee
// that frees.
#include "recall.h"

struct node {
  struct node *next;
  int value;
};

static void destroy(struct node *n) { free(n); }

void bad_alias(void) {
  char *data = malloc(100);
  if (!data)
    return;
  char *alias = data;
  free(data);
  alias[0] = 'A'; // RECALL: use-after-free
}

void bad_callee(void) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return;
  n->value = 1;
  destroy(n);
  print_int(n->value); // RECALL: use-after-free
}

void bad_field_pointer(void) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return;
  int *v = &n->value;
  free(n);
  *v = 2; // RECALL: use-after-free
}

void bad_list_walk(struct node *head) {
  struct node *n = head;
  while (n) {
    free(n);
    n = n->next; // RECALL: use-after-free
  }
}
