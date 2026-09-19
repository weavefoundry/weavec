/* rewrite-oracle-nonnull-contexts.c as the check emitter rewrites it. */
struct pair { int a, b; };
struct pair make(int *p) {
  struct pair r = {*(int *)__weavec_chk_nonnull(p), ((int *)__weavec_chk_nonnull(p))[1]};
  return r;
}
int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n && *(int *)__weavec_chk_nonnull(p); i++)
    s += *(int *)__weavec_chk_nonnull(p);
  if (*(int *)__weavec_chk_nonnull(p) > 3)
    return s;
  return -*(int *)__weavec_chk_nonnull(p);
}
