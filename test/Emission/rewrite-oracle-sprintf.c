// RFC 0030, section 10.6, gate G8: the sprintf lowering: snprintf bounded by the destination, its result checked (len, result form).
// The -O0 IR equals that of Inputs/rewrite-oracle-sprintf.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-sprintf.expected.c %t

int sprintf(char *restrict, const char *restrict, ...);
int snprintf(char *restrict, unsigned long, const char *restrict, ...);
int format(int x) {
  char buf[8];
  return sprintf(buf, "%d", x);
}
