/* rewrite-oracle-checks-none.c as the check emitter rewrites it. */
void *malloc(unsigned long);
int load(int *p, int i) {
  int a[4];
  int *q = malloc(4);
  a[i] = *p;
  return a[i] + *q;
}
