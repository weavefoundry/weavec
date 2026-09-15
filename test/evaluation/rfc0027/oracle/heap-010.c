#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
static void destroy(struct node *p) {
  if (!p) return;
  destroy(p->left); destroy(p->right); free(p);
}
int main(void) {
  return 0;
}
