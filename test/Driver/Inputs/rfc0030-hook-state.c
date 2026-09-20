// The unit that stores the allocator (lstate.c's `lua_newstate`) and calls
// through it (`luaM_free`).
#include "rfc0030-hook-state.h"
static struct global_state the_state;
struct global_state *new_state(alloc_fn f, void *ud) {
  the_state.frealloc = f;
  the_state.ud = ud;
  return &the_state;
}
void state_release(struct global_state *g, void *block, size_t size) {
  (*g->frealloc)(g->ud, block, size, 0);
}
