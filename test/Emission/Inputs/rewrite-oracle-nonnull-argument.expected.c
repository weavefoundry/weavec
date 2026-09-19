/* rewrite-oracle-nonnull-argument.c as the check emitter rewrites it. */
unsigned long strlen(const char *);
unsigned long len(const char *s) { return strlen((const char *)__weavec_chk_nonnull(s)); }
