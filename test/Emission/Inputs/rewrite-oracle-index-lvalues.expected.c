/* rewrite-oracle-index-lvalues.c as the check emitter rewrites it. */
int fill(int i, int j, int k, int m, int v) {
  int a[8] = {0};
  a[__weavec_chk_index(i, 8)] = v;
  a[__weavec_chk_index(j, 8)]++;
  a[__weavec_chk_index(k, 8)] += v;
  *(a + __weavec_chk_index(m, 8)) = v;
  return v;
}
