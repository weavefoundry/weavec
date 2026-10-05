/* rewrite-oracle-lowered-violation.c as the check emitter rewrites it, with
 * the runtime. */
void free(void *);
void twice(int *p) {
  free(__weavec_chk_release(p));
  free(__weavec_chk_release(p));
}
