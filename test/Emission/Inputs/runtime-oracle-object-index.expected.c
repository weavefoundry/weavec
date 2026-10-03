/* runtime-oracle-object-index.c as the check emitter rewrites it. */
struct vec {
  int *data;
  unsigned long n;
};

int at(struct vec *v, unsigned long i) {
  return *(int *)__weavec_chk_object(
      (int *)__weavec_chk_nonnull(((struct vec *)__weavec_chk_nonnull(v))->data), (i), 4ULL, 0ULL,
      4ULL);
}
void put(struct vec *v, unsigned long i, int x) {
  *(int *)__weavec_chk_object(
      (int *)__weavec_chk_nonnull(((struct vec *)__weavec_chk_nonnull(v))->data), (i), 4ULL, 0ULL,
      4ULL) = x;
}
