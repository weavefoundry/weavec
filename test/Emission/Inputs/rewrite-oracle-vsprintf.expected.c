/* rewrite-oracle-vsprintf.c as the check emitter rewrites it. */
typedef __builtin_va_list va_list;
int vsprintf(char *restrict, const char *restrict, va_list);
int vsnprintf(char *restrict, unsigned long, const char *restrict, va_list);
int vformat(va_list ap) {
  char buf[12];
  return __weavec_chk_len_r(vsnprintf(buf, sizeof(char[12]), "%x-%x", ap), sizeof(char[12]));
}
