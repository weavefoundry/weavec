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
  struct node *p1=malloc(sizeof *p1);
  if (!p1) { free(p0); return 0; }
  *p1=(struct node){2,0,0};
  struct node *p2=malloc(sizeof *p2);
  if (!p2) { free(p0); free(p1); return 0; }
  *p2=(struct node){3,0,0};
  p0->left=p2;
  p0->right=p1;
  destroy(p0);
  destroy(p0);
  return 0;
}
