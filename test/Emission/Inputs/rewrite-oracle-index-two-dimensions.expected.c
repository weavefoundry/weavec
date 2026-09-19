/* rewrite-oracle-index-two-dimensions.c as the check emitter rewrites it. */
int cell(int i, int j) {
  int m[3][4] = {{0}};
  return m[__weavec_chk_index(i, 3)][__weavec_chk_index(j, 4)];
}
