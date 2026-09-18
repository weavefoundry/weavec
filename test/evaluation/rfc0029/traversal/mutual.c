#include <stdlib.h>
struct tree { unsigned value; struct tree *left, *right; };
unsigned visit_odd(const struct tree *);
unsigned visit_even(const struct tree *p) {
  if (!p) return 0;
  return p->value + visit_odd(p->left) + visit_odd(p->right);
}
unsigned visit_odd(const struct tree *p) {
  if (!p) return 0;
  return p->value + visit_even(p->left) + visit_even(p->right);
}
int main(void) {
  struct tree b = {2, 0, 0}, a = {1, &b, 0};
  return visit_even(&a) != 3;
}
