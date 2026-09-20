// RFC 0030, section 10.6, gate G8: strdup and strndup become zeroing wrappers; their argument is still checked.
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-strings.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-strings.expected.c %t

char *strdup(const char *);
char *strndup(const char *, unsigned long);
char *dup(const char *s) { return strdup(s); }
char *dupn(const char *s, unsigned long n) { return strndup(s, n); }
