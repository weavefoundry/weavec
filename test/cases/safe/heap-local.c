// RFC 0041 §6: a local whose address escapes is placed on the heap; the
// program runs as written.
// CLEAN
// FLAGS: -O2
#include <weavec.h>

struct node { struct node *next; int v; };
static struct node *head;

WEAVEC_SAFE void push(struct node *WEAVEC_ESCAPES n) {
  n->next = head;
  head = n;
}

WEAVEC_SAFE int build(int count) {
  int total = 0;
  for (int i = 0; i < count; i++) {
    struct node local = {0, i};
    push(&local);
    total += head->v;
  }
  head = 0;
  return total;
}

int main(void) { return build(10) == 45 ? 0 : 1; }
