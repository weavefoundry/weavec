/* rewrite-oracle-zero-init-strings.c as the check emitter rewrites it. */
char *strdup(const char *);
char *strndup(const char *, unsigned long);
char *dup(const char *s) { return __weavec_strdup_zero((const char *)__weavec_chk_nonnull(s)); }
char *dupn(const char *s, unsigned long n) {
  return __weavec_strndup_zero((const char *)__weavec_chk_nonnull(s), n);
}
