/* rewrite-oracle-span-vla.c as the check emitter rewrites it. */
int get(int n, int i) {
  int v[n];
  v[0] = 1;
  return v[i];
}

int get_positive(int n, int i) {
  if (n < 1)
    return 0;
  int v[n];
  v[0] = 1;
  return *(int *)__weavec_chk_span(v, i, v, sizeof(v), sizeof(int));
}
