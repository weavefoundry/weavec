// A unit that defines the allocator (RFC 0030 §11): its `malloc` answers no
// usable-size query the zero-initialisation wrappers of other units use.
#include <stddef.h>
static char arena[1 << 12];
static size_t used;
void *malloc(size_t size) {
  if (size > sizeof arena - used)
    return NULL;
  void *block = &arena[used];
  used += (size + 15) & ~(size_t)15;
  return block;
}
void free(void *block) { (void)block; }
