/* runtime-oracle-range-cache.c as the check emitter rewrites it. */
struct vec {
  int *data;
  unsigned long n;
};
extern void unknown(void);

int quiet(struct vec *v) {
  unsigned long long __weavec_ranges[4];
  __builtin_memset(__weavec_ranges, 0, sizeof __weavec_ranges);
  int s = 0;
  {
    __weavec_range_check(__weavec_ranges);
    for (unsigned long i = 0; i < ((struct vec *)__weavec_chk_nonnull(v))->n; i++)
      s += *(int *)__weavec_chk_object_c((int *)__weavec_chk_nonnull(v->data), (i), 4ULL, 0ULL,
                                         4ULL, __weavec_ranges, 1);
  }
  return s;
}

int calls(struct vec *v) {
  unsigned long long __weavec_ranges[4];
  __builtin_memset(__weavec_ranges, 0, sizeof __weavec_ranges);
  int s = 0;
  for (unsigned long i = 0; i < ((struct vec *)__weavec_chk_nonnull(v))->n; i++) {
    s += *(int *)__weavec_chk_object_c((int *)__weavec_chk_nonnull(v->data), (i), 4ULL, 0ULL,
                                       4ULL, __weavec_ranges, 0);
    unknown();
  }
  return s;
}
