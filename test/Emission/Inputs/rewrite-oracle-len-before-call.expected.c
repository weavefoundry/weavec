/* rewrite-oracle-len-before-call.c as the check emitter rewrites it. */
void *memcpy(void *, const void *, unsigned long);
void copy(unsigned long n) {
  char d[16];
  char s[16] = {0};
  (__weavec_chk_len(n, sizeof(char[16])),
   (__weavec_chk_len(n, sizeof(char[16])), memcpy(__weavec_chk_disjoint(d, s, n), s, n)));
}
