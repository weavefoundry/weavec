// RUN: not %weavec --checked %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RUN: not %weavec --whole-program --checked %s -- -ferror-limit=0 2>&1 | FileCheck %s
// RFC 0020: a may-alias from an earlier iteration is not a null proof.

// CHECK: checked safety failed: dereference of 'bad', which is null [weavec::checking-failed]

struct node {
  struct node *next;
  struct node *prev;
};
void split(struct node *list) {
  struct node *slow = list;
  struct node *fast = list;
  if (!list || !list->next)
    return;
  while (fast) {
    slow = slow->next;
    fast = fast->next;
    if (fast)
      fast = fast->next;
  }
  if (slow && slow->prev) {
    int *bad = 0;
    *bad = 42;
    slow->prev->next = 0;
  }
}
