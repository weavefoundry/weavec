// RFC 0030 §7.2, row 4: WEAVEC_NONNULL and WEAVEC_NULLABLE declare the
// nullability of a parameter, field, result or variable, and nothing about
// its shape, which the other rows or §7.3 inference give. Both on one
// declaration is a conflict: the weaker, nullable, is used.
// RUN: %weavec --dump-kinds %s -- | FileCheck %s
#include <weavec.h>

struct node {
  // CHECK: field node.next: single nullable [inferred, lower-bound, nullability annotation]
  struct node *WEAVEC_NULLABLE next;
  // CHECK: field node.owner: single nonnull [inferred, lower-bound, nullability annotation]
  struct node *WEAVEC_NONNULL owner;
};

// CHECK: variable head: single nonnull [inferred, lower-bound, nullability annotation]
struct node *WEAVEC_NONNULL head;

// CHECK: param visit 0 'n': single nonnull [default, lower-bound, nullability annotation]
int visit(struct node *WEAVEC_NONNULL n) { return n->owner != 0; }
// CHECK: result find: single nullable [default, lower-bound, nullability annotation]
struct node *WEAVEC_NULLABLE find(int key);
// CHECK: result root: single nonnull [default, lower-bound, nullability annotation]
WEAVEC_NONNULL struct node *root(void);
// CHECK: param torn 0 'both': unknown nullable [declared, nullability annotation]
// CHECK-NEXT: problem [[@LINE+1]]:55: conflicting kinds for 'both': nonnull and nullable
void torn(struct node *WEAVEC_NONNULL WEAVEC_NULLABLE both);

int use(void) {
  torn(head);
  return visit(find(1)) + visit(root());
}
