// RUN: %weavec --checked-function=main %s -- 2>&1 | FileCheck %s --allow-empty
// RFC 0023: runtime length is represented by a preserved invariant.
// CHECK-NOT: error:
#include <stdlib.h>
struct node { unsigned value; struct node *next; };
static struct node *build(unsigned count) {
  struct node *head = NULL;
  for (unsigned i = 0; i < count; ++i) {
    struct node *p = malloc(sizeof *p);
    if (!p) break;
    p->value = i;
    p->next = head;
    head = p;
  }
  return head;
}
static void destroy(struct node *p) {
  while (p) {
    struct node *next = p->next;
    free(p);
    p = next;
  }
}
int main(int argc, char **argv) {
  (void)argv;
  destroy(build((unsigned)argc));
  return 0;
}
