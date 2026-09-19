struct node { int v; struct node *next; };
int main(void) {
  struct node n;
  n.v = 1;
  return n.next->v; // BUG: use-of-uninitialized
}
