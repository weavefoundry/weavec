// Salvaged false proof (RFC 0030 section 17.2): rfc0029/offsets, case recursive-offset-forward (candidate 17).
// even() is applied to p + 1, one element past the calloc block: it reads and frees past the object.
#include <stdlib.h>
struct tree { struct tree *left, *right; };
static void odd(struct tree *p);
static void even(struct tree *p) {
  if(!p) return;
  odd(p->left); odd(p->right); free(p); // NOT-PROVEN: spatial
}
static void odd(struct tree *p) { even(p+1); }
int main(void) {
  struct tree *p=calloc(1,sizeof *p); if(!p)return 0;
  odd(p); return 0; // BUG: invalid-release
}
