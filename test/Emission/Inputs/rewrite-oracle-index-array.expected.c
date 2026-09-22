/* rewrite-oracle-index-array.c as the check emitter rewrites it. */
int get(int i) {
  int a[10] = {0};
  return a[__weavec_chk_index(i, 10)];
}
void put(int i, int v) {
  int a[10] = {0};
  a[__weavec_chk_index(i, 10)] = v;
}
