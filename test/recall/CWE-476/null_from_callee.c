// CWE-476: NULL pointer dereference, a callee's result that may be NULL is
// dereferenced without a check, directly and through a wrapper.
#include "recall.h"

struct node {
  int value;
};

static struct node *make_node(int value) {
  struct node *n = malloc(sizeof *n);
  if (n)
    n->value = value;
  return n;
}

void bad(void) {
  struct node *n = make_node(1);
  print_int(n->value); // RECALL: null-dereference
  free(n);
}

void bad_passed_on(void) {
  struct node *n = make_node(1);
  puts((char *)n); // RECALL: null-dereference
  free(n);
}

void good(void) {
  struct node *n = make_node(1);
  if (!n)
    return;
  print_int(n->value);
  free(n);
}
