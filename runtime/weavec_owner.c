/*===- weavec_owner.c - One runtime per process -----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0033, section 6.2. This copy's table of entry points, and on Darwin
|* the choice of the copy that owns the process: the table
|* `dlsym(RTLD_DEFAULT, ...)` finds first, which every image finds alike.
|* A copy that is not the owner forwards to it and takes its arena's
|* descriptor, so one arena, one object table and one quarantine serve every
|* image. A table of another layout (a runtime of another WeaveC version) is
|* not used: that image keeps its own runtime.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <string.h>

enum { WeavecRtDispatchMagic = 0x52435657, WeavecRtDispatchVersion = 1 };

const struct __weavec_rt_dispatch __weavec_rt_dispatch = {
    WeavecRtDispatchMagic,      WeavecRtDispatchVersion,
    &__weavec_rt_heap,          weavecRtInitialise,
    __weavec_rt_alloc,          __weavec_rt_free,
    __weavec_rt_realloc,        __weavec_rt_size,
    __weavec_rt_find,           __weavec_rt_object,
    __weavec_rt_string,         __weavec_rt_strlen,
    __weavec_rt_live,           __weavec_rt_release_ok,
    __weavec_rt_object_range,   __weavec_rt_live_range,
    __weavec_rt_stack_enter,    __weavec_rt_stack_leave,
    __weavec_rt_stack_rewind,   __weavec_rt_globals_add,
    __weavec_rt_report,         __weavec_rt_fatal};

#if defined(__APPLE__)

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>

enum { Unresolved = 0, Resolving = 1, Resolved = 2 };

static const struct __weavec_rt_dispatch *owner;
static unsigned ownerState = Unresolved;
/* The thread finding the owner: what it calls meanwhile (dlsym may
 * allocate) is served here. Not a thread-local variable: Darwin allocates
 * those on first use, through malloc. */
static pthread_t resolver;

const struct __weavec_rt_dispatch *weavecRtForward(void) {
  unsigned state = __atomic_load_n(&ownerState, __ATOMIC_ACQUIRE);
  unsigned expected = Unresolved;
  const struct __weavec_rt_dispatch *found;
  if (__builtin_expect(state == Resolved, 1))
    return owner;
  if (state == Resolving && pthread_equal(resolver, pthread_self()))
    return 0;
  if (!__atomic_compare_exchange_n(&ownerState, &expected, Resolving, 0,
                                   __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&ownerState, __ATOMIC_ACQUIRE) != Resolved)
      sched_yield();
    return owner;
  }
  resolver = pthread_self();
  found = (const struct __weavec_rt_dispatch *)dlsym(RTLD_DEFAULT,
                                                     "__weavec_rt_dispatch");
  if (found != 0 && found != &__weavec_rt_dispatch &&
      found->magic == WeavecRtDispatchMagic &&
      found->version == WeavecRtDispatchVersion && found->initialise()) {
    /* The owner's arena is fixed once reserved: its descriptor serves this
     * image's inline guards; its base, published last, opens it. */
    const uintptr_t base = found->heap->base;
    struct __weavec_rt_heap_t copy;
    memcpy(&copy, found->heap, sizeof copy);
    copy.base = 0;
    memcpy(&__weavec_rt_heap, &copy, sizeof copy);
    __atomic_store_n(&__weavec_rt_heap.base, base, __ATOMIC_RELEASE);
    owner = found;
  }
  __atomic_store_n(&ownerState, Resolved, __ATOMIC_RELEASE);
  return owner;
}

/* Before this image's own code runs, as far as an initialiser can be. The
 * owner reserves its arena and promotes its zone here, so that the C
 * library's allocations in the program's first statements are in it. */
__attribute__((constructor)) static void resolveOwner(void) {
  if (weavecRtForward() == 0)
    (void)weavecRtInitialise();
}

#endif
