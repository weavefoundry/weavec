#include <stdlib.h>
struct tree { struct tree *left, *right; };
static void odd(struct tree *p);
static void even(struct tree *p) {
  if(!p) return;
  odd(p->left); odd(p->right); free(p);
}
static void odd(struct tree *p) { even(p+0); }
int main(void) {
  struct tree *p=calloc(1,sizeof *p); if(!p)return 0;
  odd(p); return 0;
}
