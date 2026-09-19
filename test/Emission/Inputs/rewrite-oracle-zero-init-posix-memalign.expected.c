/* rewrite-oracle-zero-init-posix-memalign.c as the check emitter rewrites it. */
int posix_memalign(void **, unsigned long, unsigned long);
void *get(unsigned long n) {
  void *p = 0;
  if (__weavec_posix_memalign_zero(posix_memalign, &p, 64, n) != 0)
    return 0;
  return p;
}
