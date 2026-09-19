/* rewrite-oracle-len-memset.c as the check emitter rewrites it. */
void *memset(void *, int, unsigned long);
void clear(unsigned long n) {
  char b[32];
  (__weavec_chk_len(n, sizeof(char[32])), memset(b, 0, n));
  (__weavec_chk_len(32, sizeof(char[32])), memset(b, 1, sizeof b));
}
