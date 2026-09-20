/* rewrite-oracle-sprintf.c as the check emitter rewrites it. */
int sprintf(char *restrict, const char *restrict, ...);
int snprintf(char *restrict, unsigned long, const char *restrict, ...);
int format(int x) {
  char buf[8];
  return __weavec_chk_len_r(snprintf(buf, sizeof(char[8]), "%d", x), sizeof(char[8]));
}
