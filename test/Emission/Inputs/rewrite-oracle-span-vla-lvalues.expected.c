/* rewrite-oracle-span-vla-lvalues.c as the check emitter rewrites it. */
long put(int n, int i, long x) {
  long v[n];
  *(long *)__weavec_chk_span(v, i, v, sizeof(v), sizeof(long)) = x;
  (*(long *)__weavec_chk_span(v, i, v, sizeof(v), sizeof(long)))++;
  *(long *)__weavec_chk_span(v, i, v, sizeof(v), sizeof(long)) += x;
  *(long *)__weavec_chk_span(v, i, v, sizeof(v), sizeof(long)) = x;
  return x;
}
