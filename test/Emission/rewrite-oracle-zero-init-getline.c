// RFC 0030, section 10.6, gate G8: getline's buffer is zeroed after the line it read.
// `&line` and `&cap` are non-null and each points to one whole object, so only
// the stream is checked.
// The -O0 IR equals that of Inputs/rewrite-oracle-zero-init-getline.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-zero-init-getline.expected.c %t

typedef struct FILE FILE;
long getline(char **, unsigned long *, FILE *);
long readline(FILE *f) {
  char *line = 0;
  unsigned long cap = 0;
  return getline(&line, &cap, f);
}
