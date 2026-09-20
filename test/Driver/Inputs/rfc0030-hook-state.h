// Lua's allocator hook, reduced: the state keeps the allocator it was made
// with in a function-pointer field, and frees through it.
#ifndef RFC0030_HOOK_STATE_H
#define RFC0030_HOOK_STATE_H
#include <stddef.h>
typedef void *(*alloc_fn)(void *ud, void *ptr, size_t osize, size_t nsize);
struct global_state {
  alloc_fn frealloc;
  void *ud;
};
struct global_state *new_state(alloc_fn f, void *ud);
void state_release(struct global_state *g, void *block, size_t size);
#endif
