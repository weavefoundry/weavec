/* rewrite-oracle-lowered-violation.c as the check emitter rewrites it,
 * without the runtime: nothing. */
void free(void *);
void twice(int *p) {
  free(p);
  free(p);
}
