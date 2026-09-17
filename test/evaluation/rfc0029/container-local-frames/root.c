#include <string.h>
struct node { unsigned value; struct node *left, *right; };
struct state { unsigned depth, mode; };
unsigned walk(const struct node *p) {
  if (!p) return 0;
  return p->value + walk(p->left) + walk(p->right);
}
unsigned inspect(struct node *p) {
  struct state state[1];
  memset(p, 0xa5, sizeof(*p));
  state->depth = 1;
  return walk(p) + state->depth;
}
int main(void) {
  struct node child = {2, 0, 0}, root = {1, &child, 0};
  return inspect(&root) != 4;
}
