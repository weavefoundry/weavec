/* rewrite-oracle-len-before-call.c as the check emitter rewrites it. */
void *memcpy(void *, const void *, unsigned long);
void copy(unsigned long n) {
  char b[32] = {0};
  char *s = b + 8;
  (__weavec_chk_len(n, __weavec_have_sub(sizeof(char[32]), 8)),
   (__weavec_chk_len(n, sizeof(char[32])), memcpy(__weavec_chk_disjoint(b, s, n), s, n)));
}
