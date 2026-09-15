#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
static void destroy(struct node *p) {
  if (!p) return;
  destroy(p->left); destroy(p->right); free(p);
}
int main(void) {
  struct node *p0=malloc(sizeof *p0);
  if (!p0) { return 0; }
  *p0=(struct node){1,0,0};
  destroy(p0);
  return 0;
}
