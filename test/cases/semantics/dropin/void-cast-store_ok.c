// RFC 0033 §1 (a definite error needs a witness): a callee's store of a value
// read through a `void *` (yyjson's `unsafe_yyjson_get_str`) is a store in its
// summary, so the caller does not keep the null its local held before
// (yyjson's test_json_pointer.c: a false null-dereference error).
// STAGE: S8
// CLEAN
// UNITS: Inputs/void-cast-lookup.c
// RUN-INPUT:
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
typedef struct V { unsigned long tag; union { const char *str; } uni; } V;
V *getx_impl(V *val, const char *ptr, size_t len, void *err);
static inline V *getn(V *val, const char *ptr, size_t len) { return getx_impl(val, ptr, len, NULL); }
static inline V *get(V *val, const char *ptr) { if (!ptr) return NULL; return getn(val, ptr, strlen(ptr)); }
static inline bool is_str(V *v) { return v ? (v->tag & 7) == 5 : false; }
static inline const char *ustr(void *v) { return ((V *)v)->uni.str; }
static inline bool get_str(V *root, const char *ptr, const char **value) {
  V *val = get(root, ptr);
  if (value && is_str(val)) { *value = ustr(val); return true; } else { return false; }
}
static void check(V *root) {
  const char *s;
  if (!(get_str(root, "/pistr", &s) == true && strcmp(s, "3.14159") == 0)) abort();
}
int main(void) {
  V root = {5, {"3.14159"}};
  check(&root);
  return 0;
}
