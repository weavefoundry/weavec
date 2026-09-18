#include <stdlib.h>
struct tree { struct tree *left, *right; };
static void destroy(struct tree *p) {
  if (!p) return;
  if (p->left) destroy(p->left + 1);
  destroy(p->right);
  free(p);
}
int main(void) {
  struct tree *p=calloc(1,sizeof *p); if(!p)return 0;
  struct tree *q=calloc(1,sizeof *q); if(!q){free(p);return 0;}
  p->left=q; destroy(p); return 0;
}
