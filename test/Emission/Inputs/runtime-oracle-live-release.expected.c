/* runtime-oracle-live-release.c as the check emitter rewrites it. */
struct vec {
  int *data;
  unsigned long n;
};
extern void unknown(void);
extern void free(void *);
extern int *G;

int after(void) {
  int *p = G;
  unknown();
  return *(int *)__weavec_chk_live((int *)__weavec_chk_nonnull(p));
}

void drop(struct vec *v) {
  free(__weavec_chk_release(((struct vec *)__weavec_chk_nonnull(v))->data));
}
