// RFC 0042 §6: the typed unit that defines what typed-units.c calls.
#include <weavec.h>
#include <stddef.h>

struct vec { int *WEAVEC_COUNTED_BY(cap) data; size_t cap; };

int sum(const struct vec *v) {
  int s = 0;
  for (size_t i = 0; i < v->cap; i++)
    s += v->data[i];
  return s;
}
