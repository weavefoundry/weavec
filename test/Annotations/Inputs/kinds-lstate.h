/* A shape of Lua's lstate.h for the RFC 0030 §9.3 slot tests: the state and
 * its allocator slot are declared in a header, so other units may store
 * other allocators into it. */
#ifndef WEAVEC_TEST_KINDS_LSTATE_H
#define WEAVEC_TEST_KINDS_LSTATE_H

typedef unsigned long size_t;
typedef void *(*lua_Alloc)(void *ud, void *ptr, size_t osize, size_t nsize);

struct global_State {
  lua_Alloc frealloc;
  void *ud;
};

struct global_State *new_state(void);

#endif /* WEAVEC_TEST_KINDS_LSTATE_H */
