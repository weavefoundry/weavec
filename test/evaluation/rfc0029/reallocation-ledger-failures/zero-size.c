#include <stdlib.h>
struct node { unsigned value; struct node *left, *right; };
unsigned walk(const struct node *p) {
  if (!p) return 0;
  return p->value + walk(p->left) + walk(p->right);
}
unsigned char *make(const struct node *p) {
  (void)walk(p);
  unsigned char *data = malloc(4);
  if (!data) return 0;
  data[0] = 0;
  unsigned char *out = realloc(data, 0); return out;
}
int main(void) {
  struct node child = {2,0,0}, root = {1,&child,0};
  unsigned char *data = make(&root);
  free(data); return 0;
}
