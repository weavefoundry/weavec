/* rewrite-oracle-assume.c as the check emitter rewrites it. */
int positive(int n) {
  __weavec_chk_assert((n > 0) != 0);
  return n;
}
