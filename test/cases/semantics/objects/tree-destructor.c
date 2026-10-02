// RFC 0031 §4.5 D3, §6.4: a recursive tree destructor is proven.
// STAGE: S4
// 'left' and 'right' are owning slots, so the subtrees freed by the recursive calls are
// distinct from 't' and from each other (D2, D3): reading 't->right' after freeing the left
// subtree, and freeing 't' last, are proven.
// CLEAN
// ASAN
// EXPECT-LEDGER: /summary/facets/temporal/unresolved == 0
#include <stdlib.h>
struct tree { struct tree *left, *right; int v; };
void free_tree(struct tree *t) {
  if (!t) return;
  free_tree(t->left);
  free_tree(t->right);
  free(t);
}
static struct tree *build(int depth) {
  if (depth == 0) return NULL;
  struct tree *t = malloc(sizeof *t);
  if (!t) abort();
  t->v = depth;
  t->left = build(depth - 1);
  t->right = build(depth - 1);
  return t;
}
int main(void) {
  free_tree(build(3));
  return 0;
}
