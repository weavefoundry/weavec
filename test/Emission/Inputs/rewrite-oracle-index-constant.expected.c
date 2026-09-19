/* rewrite-oracle-index-constant.c as the check emitter rewrites it. */
int third(void) {
  int a[4] = {1, 2, 3, 4};
  return a[__weavec_chk_index(2, 4)];
}
