/* rewrite-oracle-index-lvalues.c as the check emitter rewrites it. */
int fill(int i, int v) {
  int a[8] = {0};
  a[__weavec_chk_index(i, 8)] = v;
  a[__weavec_chk_index(i, 8)]++;
  a[__weavec_chk_index(i, 8)] += v;
  *(a + __weavec_chk_index(i, 8)) = v;
  return a[__weavec_chk_index(0, 8)];
}
