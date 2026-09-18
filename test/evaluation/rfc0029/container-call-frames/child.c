#include <stdlib.h>
#include <string.h>
struct node { unsigned value; struct node *left, *right; };
struct state { unsigned depth, mode; unsigned char *data; };
unsigned walk(const struct node *p) {
  if (!p) return 0;
  return p->value + walk(p->left) + walk(p->right);
}
void set(struct state *s) { s->depth = 1; s->data[0] = 7; }
void corrupt(struct node *p) { memset(p, 0xa5, sizeof(*p)); }
unsigned inspect(struct node *p) {
  struct state state[1];
  memset(state, 0, sizeof(state));
  state->data = malloc(1);
  if (!state->data) return 0;
  set(state); corrupt(p->left);
  unsigned value = walk(p) + state->depth;
  free(state->data);
  return value;
}
int main(void) {
  struct node child = {2, 0, 0}, root = {1, &child, 0};
  (void)inspect(&root); return 0;
}
