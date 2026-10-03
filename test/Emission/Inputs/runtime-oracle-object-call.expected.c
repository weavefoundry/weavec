/* runtime-oracle-object-call.c as the check emitter rewrites it. */
struct vec {
  int *data;
  unsigned long n;
  char *name;
};
extern void *memset(void *, int, unsigned long);
extern unsigned long strlen(const char *);

void clear(struct vec *v, unsigned long n) {
  memset(__weavec_chk_object_n(__weavec_chk_nonnull_n(((struct vec *)__weavec_chk_nonnull(v))->data, n), n), 0, n);
}
unsigned long len(struct vec *v) {
  return strlen(__weavec_chk_object_s(__weavec_chk_nonnull(((struct vec *)__weavec_chk_nonnull(v))->name)));
}
