/* runtime-oracle-checked-wrappers.c as the check emitter rewrites it. */
extern char *strcpy(char *, const char *);
extern void *memcpy(void *, const void *, unsigned long);
extern int sprintf(char *, const char *, ...);
extern int vsprintf(char *, const char *, __builtin_va_list);
extern const char *name(void);

int show(char *out, int port) {
  return __weavec_chk_sprintf((char *)__weavec_chk_nonnull(out), "%d", 0xffffffffffffffffULL, 1u, port);
}
void copy(char *d) {
  __weavec_chk_strcpy((char *)__weavec_chk_nonnull(d), (const char *)__weavec_chk_object_s(__weavec_chk_nonnull(name())), 0xffffffffffffffffULL, 3u);
}
void shift(char *p) {
  __weavec_chk_strcpy((char *)__weavec_chk_nonnull(p), (const char *)__weavec_chk_object_s(p + 2), 0xffffffffffffffffULL, 3u);
}
void slide(char *a, unsigned long n) {
  __weavec_chk_memcpy(__weavec_chk_object_n(__weavec_chk_nonnull_n(a, n), n), __weavec_chk_object_n(a + 1, n), n, 0xffffffffffffffffULL, 2u);
}
int vshow(char *out, __builtin_va_list ap) {
  return __weavec_chk_vsprintf((char *)__weavec_chk_nonnull(out), "%d", ap, 0xffffffffffffffffULL, 1u);
}
void fortified(char *d, int port) {
  __weavec_chk_strcpy((char *)__weavec_chk_nonnull(d), (const char *)__weavec_chk_object_s(__weavec_chk_nonnull(name())), __builtin_object_size(d, 1), 3u);
  __weavec_chk_sprintf((char *)__weavec_chk_nonnull(d), "%d", __builtin_object_size(d, 1), 1u, port);
}
