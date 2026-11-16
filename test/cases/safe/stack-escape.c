// RFC 0041 §5: an escaping parameter must not point into the stack; RFC
// 0048 §7: its entry checks it, at the call that passes it.
// FLAGS: -O1
#include <weavec.h>

struct node { struct node *next; int v; };
static struct node *head;

WEAVEC_SAFE void push(struct node *WEAVEC_ESCAPES n) {
  n->next = head;
  head = n;
}

int main(void) {
  struct node mine = {0, 1};
  push(&mine); // TRAP: stack-escape
  return head->v;
}
