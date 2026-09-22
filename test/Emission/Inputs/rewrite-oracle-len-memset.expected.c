/* rewrite-oracle-len-memset.c as the check emitter rewrites it. */
void *memset(void *, int, unsigned long);
void clear(unsigned long n, unsigned long m) {
  char b[32];
  int w[8];
  (__weavec_chk_len(n, sizeof(char[32])), memset(b, 0, n));
  (__weavec_chk_len(m, sizeof(int[8])), memset(w, 1, m));
}
