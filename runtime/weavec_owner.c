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
|* arena's and shadow's descriptors, so one arena, one shadow and one
|* quarantine serve every image. A table of another layout (a runtime of another WeaveC version) is
|* not used: that image keeps its own runtime.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <string.h>

enum { WeavecRtDispatchMagic = 0x52435657, WeavecRtDispatchVersion = 3 };

const struct __weavec_rt_dispatch __weavec_rt_dispatch = {
    WeavecRtDispatchMagic, WeavecRtDispatchVersion, &__weavec_rt_heap,
    &__weavec_rt_shadow,   weavecRtInitialise,      __weavec_rt_alloc,
    __weavec_rt_free,      __weavec_rt_realloc,     __weavec_rt_size,
    weavecRtHeapObjectAt,  __weavec_rt_fatal};

#if defined(__APPLE__)

#include <dlfcn.h>
#include <pthread.h>
#include <sched.h>

enum { Unresolved = 0, Resolving = 1, Resolved = WeavecRtOwnerResolved };

const struct __weavec_rt_dispatch *weavecRtOwner;
unsigned weavecRtOwnerState = Unresolved;
/* The thread finding the owner: what it calls meanwhile (dlsym may
 * allocate) is served here. Not a thread-local variable: Darwin allocates
 * those on first use, through malloc. */
static pthread_t resolver;

const struct __weavec_rt_dispatch *weavecRtResolveOwner(void) {
  unsigned state = __atomic_load_n(&weavecRtOwnerState, __ATOMIC_ACQUIRE);
  unsigned expected = Unresolved;
  const struct __weavec_rt_dispatch *found;
  if (state == Resolved)
    return weavecRtOwner;
  if (state == Resolving && pthread_equal(resolver, pthread_self()))
    return 0;
  if (!__atomic_compare_exchange_n(&weavecRtOwnerState, &expected, Resolving,
                                   0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
    while (__atomic_load_n(&weavecRtOwnerState, __ATOMIC_ACQUIRE) != Resolved)
      sched_yield();
    return weavecRtOwner;
  }
  resolver = pthread_self();
  found = (const struct __weavec_rt_dispatch *)dlsym(RTLD_DEFAULT,
                                                     "__weavec_rt_dispatch");
  if (found != 0 && found != &__weavec_rt_dispatch &&
      found->magic == WeavecRtDispatchMagic &&
      found->version == WeavecRtDispatchVersion && found->initialise()) {
    /* The owner's arena and shadow are fixed once reserved: its shadow
     * descriptor serves this image's inline guards and frames. `mask` and
     * `bytes`, published last, open them. */
    struct __weavec_rt_heap_t copy;
    memcpy(&copy, found->heap, sizeof copy);
    copy.bytes = 0;
    memcpy(&__weavec_rt_heap, &copy, sizeof copy);
    __atomic_store_n(&__weavec_rt_heap.bytes, found->heap->bytes,
                     __ATOMIC_RELEASE);
    __weavec_rt_shadow.bits = found->shadow->bits;
    __atomic_store_n(&__weavec_rt_shadow.base, found->shadow->base,
                     __ATOMIC_RELEASE);
    __atomic_store_n(&__weavec_rt_shadow.mask, found->shadow->mask,
                     __ATOMIC_RELEASE);
    weavecRtOwner = found;
  }
  __atomic_store_n(&weavecRtOwnerState, Resolved, __ATOMIC_RELEASE);
  return weavecRtOwner;
}

/* Before this image's own code runs, as far as an initialiser can be. The
 * owner reserves its arena and promotes its zone here, so that the C
 * library's allocations in the program's first statements are in it. */
__attribute__((constructor)) static void resolveOwner(void) {
  if (weavecRtForward() == 0)
    (void)weavecRtInitialise();
}

#endif
