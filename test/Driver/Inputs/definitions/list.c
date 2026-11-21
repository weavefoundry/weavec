struct node {
  struct node *next;
  long value;
};
long first(long seed);
#ifdef SHARED
long shared_value(const struct node *n);
#endif
int main(void) {
  struct node n = {0, 2};
#ifdef SHARED
  if (shared_value(&n) != 2)
    return 1;
#endif
  return (int)(first(1) + n.value) - 3;
}
