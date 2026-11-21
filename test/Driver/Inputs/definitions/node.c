/* A C struct of this file's own, under a name another file uses too. */
struct node {
  long value;
};
long first(long seed) {
  struct node n = {seed};
  return n.value;
}
#ifdef SHARED
long shared_value(const struct node *n) { return n->value; }
#endif
