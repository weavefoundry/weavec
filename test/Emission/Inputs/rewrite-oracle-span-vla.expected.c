/* rewrite-oracle-span-vla.c as the check emitter rewrites it. */
int get(int n, int i) {
  int v[n];
  *(int *)__weavec_chk_span(v, 0, v, sizeof(v), sizeof(int)) = 1;
  return *(int *)__weavec_chk_span(v, i, v, sizeof(v), sizeof(int));
}
