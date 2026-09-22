// RFC 0030, section 10.6, gate G8: the vsprintf lowering over vsnprintf.
// The -O0 IR equals that of Inputs/rewrite-oracle-vsprintf.expected.c
// compiled by the reference Clang with the printed prelude.
//
// RUN: %rewrite_oracle %s %S/Inputs/rewrite-oracle-vsprintf.expected.c %t

typedef __builtin_va_list va_list;
int vsprintf(char *restrict, const char *restrict, va_list);
int vsnprintf(char *restrict, unsigned long, const char *restrict, va_list);
int vformat(va_list ap) {
  char buf[12];
  return vsprintf(buf, "%x-%x", ap);
}
