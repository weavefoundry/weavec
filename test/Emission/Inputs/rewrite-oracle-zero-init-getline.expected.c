/* rewrite-oracle-zero-init-getline.c as the check emitter rewrites it. */
typedef struct FILE FILE;
long getline(char **, unsigned long *, FILE *);
long readline(FILE *f) {
  char *line = 0;
  unsigned long cap = 0;
  return (long)__weavec_zero_line(getline(&line, &cap, (FILE *)__weavec_chk_nonnull(f)), &line);
}
