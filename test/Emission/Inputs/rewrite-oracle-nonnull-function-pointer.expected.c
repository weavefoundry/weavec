/* rewrite-oracle-nonnull-function-pointer.c as the check emitter rewrites it. */
struct ops { int (*cb)(int); };
int apply(int (*f)(int), int (*g)(int), int x) {
  return ((int (*)(int))__weavec_chk_nonnull_fn((void (*)(void))f))(x) +
         ((int (*)(int))__weavec_chk_nonnull_fn((void (*)(void))(*g)))(x);
}
int run(struct ops *o, int x) {
  return ((int (*)(int))__weavec_chk_nonnull_fn(
      (void (*)(void))((struct ops *)__weavec_chk_nonnull(o))->cb))(x);
}
