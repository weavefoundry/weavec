// The lookup void-cast-store_ok.c calls: defined in another unit, so its
// caller's analysis takes it as unknown code.
#include <stddef.h>
typedef struct V { unsigned long tag; union { const char *str; } uni; } V;
V *getx_impl(V *val, const char *ptr, size_t len, void *err) {
  (void)ptr;
  (void)len;
  (void)err;
  return val;
}
