/* rewrite-oracle-lowered-violation.c as the check emitter rewrites it. */
void free(void *);
void twice(int *p) {
  free(p);
  (__weavec_chk_violation(), free(p));
}
