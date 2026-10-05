/*===- weavec_rt.h - The WeaveC runtime's internal interface ------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0032. Shared by the runtime's own sources and its tests; not
|* installed. Compiled code reaches the runtime through the entry points
|* declared under "The object table" (the check prelude declares the same
|* ones itself, RFC 0032 section 7) and through the standard allocation
|* functions libweavec_alloc.a defines.
|*
\*===----------------------------------------------------------------------===*/

#ifndef WEAVEC_RUNTIME_WEAVEC_RT_H
#define WEAVEC_RUNTIME_WEAVEC_RT_H

#include <stddef.h>
#include <stdint.h>

/* Every entry point has default visibility: on ELF the first definition the
 * dynamic linker finds then serves every image (RFC 0032, section 2.5). */
#define WEAVEC_RT_API __attribute__((visibility("default")))

/*===-- The arena (section 2.1) -------------------------------------------===*/

enum {
  /* 16, 32, ..., 128, then four classes per doubling up to 2^30. */
  WeavecRtClassCount = 100,
  WeavecRtSmallClasses = 8,
  WeavecRtMaxClassShift = 30,
  /* A dead slot at least this large gives its pages back (section 2.4). */
  WeavecRtDecommitBytes = 64 * 1024
};

/* One size class: slot = ((offset >> shift) * magic) >> 32. */
struct __weavec_rt_class {
  uint32_t size;
  uint32_t magic;
  uint32_t shift;
  /* Slots in a region. */
  uint32_t capacity;
};

/* What a lookup of an arena pointer needs; `base` is 0 until the arena
 * exists, so every pointer is outside it. The check prelude declares the
 * same two structures (RFC 0032 section 7): its guards look an arena
 * pointer up inline. The slot words of a region are readable as soon as the
 * arena exists; a slot the region has not reached reads as never allocated. */
struct __weavec_rt_heap_t {
  uintptr_t base;
  /* classes << shift. */
  uintptr_t bytes;
  /* Region r's slot words start at meta + (r << (shift - 2)). */
  uintptr_t meta;
  uint32_t shift;
  uint32_t classes;
  struct __weavec_rt_class table[WeavecRtClassCount];
};

WEAVEC_RT_API extern struct __weavec_rt_heap_t __weavec_rt_heap;

/* Slot states: the low two bits of a slot word (section 2.2). */
enum {
  WeavecRtNever = 0,
  WeavecRtLive = 1,
  WeavecRtFree = 2,
  WeavecRtDead = 3
};

/*===-- The allocator (sections 2.3 to 2.6) -------------------------------===*/

WEAVEC_RT_API void *__weavec_rt_alloc(size_t size, size_t alignment);
WEAVEC_RT_API void __weavec_rt_free(void *p);
WEAVEC_RT_API void *__weavec_rt_realloc(void *p, size_t size);
/* The requested size of a live block; of any other pointer, what the next
 * allocator says, or 0. */
WEAVEC_RT_API size_t __weavec_rt_size(const void *p);

/*===-- The object table (sections 3 to 5) --------------------------------===*/

/* What a lookup found. */
enum { WeavecRtUntracked = 0, WeavecRtTrackedLive = 1, WeavecRtTrackedDead = 2 };
/* Where a tracked object lives. */
enum { WeavecRtHeap = 0, WeavecRtStack = 1, WeavecRtGlobal = 2 };

struct __weavec_rt_found {
  int state;
  int kind;
  uintptr_t base;
  size_t size;
  /* A stack object: the frame that entered it, and whether it was declared
   * in a nested scope of its function. */
  uintptr_t frame;
  int scoped;
  /* An arena block: its slot word and the value that makes it this block. */
  const uint32_t *word;
  uint32_t value;
};

/* The tracked object `p` points into. */
WEAVEC_RT_API struct __weavec_rt_found __weavec_rt_find(const void *p);

/* Bumped whenever a range a guard may have remembered, other than an arena
 * block, stops being valid: a huge block mapped or unmapped, a table of
 * globals added. (An arena block's validity is its own slot word.) */
WEAVEC_RT_API extern unsigned __weavec_rt_epoch;

/* Guards: 0 when the guard passes, non-zero when it fails. */
WEAVEC_RT_API int __weavec_rt_object(const void *p, long long index,
                                     unsigned long long step,
                                     unsigned long long offset,
                                     unsigned long long width);
WEAVEC_RT_API int __weavec_rt_string(const char *p);
/* The length of the string at `p`, read inside its own object: the maximum
 * when the object is dead or holds no terminator. */
WEAVEC_RT_API unsigned long long __weavec_rt_strlen(const char *p);
WEAVEC_RT_API int __weavec_rt_live(const void *p);
/* As `__weavec_rt_object` and `__weavec_rt_live`, for a guard with a
 * *range cache* (section 13): four words {lo, len, state, expect} in the
 * guarding function's frame. The answer says whether the guard failed and,
 * when it passed, the range around the pointer that keeps passing while the
 * 32-bit word at `state` reads `expect`: an arena block's bytes, valid while
 * its slot word is unchanged (the block is live and keeps its size); any
 * other object's bytes, or for an untracked pointer the part of its page no
 * tracked object can be in, valid while the epoch stands. `len` is 0, which
 * holds nothing, when there is no such range. A stack object of the
 * guarding function itself (whose frame is `frame`) is remembered only when
 * it lives as long as the frame: one declared in a nested scope may leave
 * it while the cache lives. The answer is returned by value, so the cache
 * never has its address taken and the compiler keeps it in registers. */
struct __weavec_rt_range {
  unsigned long long lo;
  unsigned long long len;
  const unsigned *state;
  unsigned long long expect;
  unsigned long long failed;
};
WEAVEC_RT_API struct __weavec_rt_range
__weavec_rt_object_range(const void *p, long long index,
                         unsigned long long step, unsigned long long offset,
                         unsigned long long width, void *frame);
WEAVEC_RT_API struct __weavec_rt_range __weavec_rt_live_range(const void *p,
                                                              void *frame);
WEAVEC_RT_API int __weavec_rt_release_ok(const void *p);

/* Stack objects (section 4). `flags`: the function has automatic objects
 * the list does not know (compound literals, `alloca`), so one may start
 * where this one ends; the object is declared in a nested scope. */
enum { WeavecRtLoose = 1, WeavecRtScoped = 2 };
WEAVEC_RT_API void *__weavec_rt_stack_enter(void *base, size_t size,
                                            void *frame, int flags);
WEAVEC_RT_API void __weavec_rt_stack_leave(void *base, void *frame);
WEAVEC_RT_API void __weavec_rt_stack_rewind(void *frame);

/* Global objects (section 5): descriptors are pairs {address, size}. */
WEAVEC_RT_API void __weavec_rt_globals_add(const void *const *begin,
                                           const void *const *end);

/*===-- Reports ------------------------------------------------------------===*/

/* RFC 0033 section 6.1: called by a failed check or guard right before it
 * traps, so that the trap ends the program even where the program blocked
 * or caught SIGTRAP and SIGILL. */
WEAVEC_RT_API void __weavec_rt_trapping(void);
WEAVEC_RT_API void __weavec_rt_report(const char *check, const char *file,
                                      unsigned line, unsigned column);
/* Prints `weavec: <what>: <why>` with the pointer and traps. */
WEAVEC_RT_API void __weavec_rt_fatal(const char *what, const void *p,
                                     const char *why)
    __attribute__((noreturn));

/*===-- One runtime per process (RFC 0033, section 6.2) --------------------===*/

/* Every image linked by weavec-cc carries a copy of the runtime. On Darwin
 * each copy would have its own arena and tables, so one of them, the one
 * `dlsym(RTLD_DEFAULT, "__weavec_rt_dispatch")` finds (the same for every
 * image), owns the process: the others forward their entry points to it
 * through its table, and copy its arena's descriptor for the guards that
 * read it inline. On ELF the dynamic linker already binds every image to
 * the first definition, and nothing forwards. */
struct __weavec_rt_dispatch {
  unsigned magic;
  unsigned version;
  const struct __weavec_rt_heap_t *heap;
  int (*initialise)(void);
  void *(*alloc)(size_t, size_t);
  void (*release)(void *);
  void *(*realloc)(void *, size_t);
  size_t (*size)(const void *);
  struct __weavec_rt_found (*find)(const void *);
  int (*object)(const void *, long long, unsigned long long, unsigned long long,
                unsigned long long);
  int (*string)(const char *);
  unsigned long long (*strlen)(const char *);
  int (*live)(const void *);
  int (*releaseOk)(const void *);
  struct __weavec_rt_range (*objectRange)(const void *, long long,
                                          unsigned long long,
                                          unsigned long long,
                                          unsigned long long, void *);
  struct __weavec_rt_range (*liveRange)(const void *, void *);
  void *(*stackEnter)(void *, size_t, void *, int);
  void (*stackLeave)(void *, void *);
  void (*stackRewind)(void *);
  void (*globalsAdd)(const void *const *, const void *const *);
  void (*report)(const char *, const char *, unsigned, unsigned);
  void (*fatal)(const char *, const void *, const char *);
};
WEAVEC_RT_API extern const struct __weavec_rt_dispatch __weavec_rt_dispatch;

#if defined(__APPLE__)
/* The owner's table when this copy forwards to it; null when this copy is
 * the owner (or while it is finding out, on the thread doing so). */
const struct __weavec_rt_dispatch *weavecRtForward(void);
#else
#define weavecRtForward() ((const struct __weavec_rt_dispatch *)0)
#endif
/* Forwards a call of this copy's entry point to the owner's. */
#define WEAVEC_RT_FORWARD(entry, ...)                                          \
  do {                                                                         \
    const struct __weavec_rt_dispatch *weavecRtOwner_ = weavecRtForward();      \
    if (weavecRtOwner_ != 0)                                                   \
      return weavecRtOwner_->entry(__VA_ARGS__);                               \
  } while (0)
#define WEAVEC_RT_FORWARD_VOID(entry, ...)                                     \
  do {                                                                         \
    const struct __weavec_rt_dispatch *weavecRtOwner_ = weavecRtForward();      \
    if (weavecRtOwner_ != 0) {                                                 \
      weavecRtOwner_->entry(__VA_ARGS__);                                      \
      return;                                                                  \
    }                                                                          \
  } while (0)

/*===-- Internal -----------------------------------------------------------===*/

/* Reserves this copy's arena on first use; 0 when there is no room. */
int weavecRtInitialise(void);

/* Whether the calling thread's stack or the global table holds `p`. */
int weavecRtIsStackOrGlobal(const void *p);
/* The stack and global parts of a lookup. */
struct __weavec_rt_found weavecRtFindStackOrGlobal(const void *p);
/* A range around an address no stack or global object is in, inside its
 * page; false when none can be given (the calling thread's stack). */
int weavecRtUntrackedRange(uintptr_t address, uintptr_t *low, uintptr_t *high);
/* Whether a huge block was ever mapped in [low, high). */
int weavecRtMayHoldHuge(uintptr_t low, uintptr_t high);

/* WEAVEC_RT_STATS=1: what the runtime did, printed when the program exits. */
enum {
  WeavecRtStatAllocations,
  WeavecRtStatReleases,
  WeavecRtStatRecycled,
  WeavecRtStatHuge,
  WeavecRtStatLookups,
  WeavecRtStatHeapLookups,
  WeavecRtStatStackLookups,
  WeavecRtStatGlobalLookups,
  WeavecRtStatUntrackedLookups,
  WeavecRtStatRanges,
  WeavecRtStatRangesKept,
  WeavecRtStatStackEnters,
  WeavecRtStatCount
};
extern unsigned long long weavecRtStats[WeavecRtStatCount];
static inline void weavecRtCount(int stat) { ++weavecRtStats[stat]; }

static inline void weavecRtBumpEpoch(void) {
  (void)__atomic_add_fetch(&__weavec_rt_epoch, 1, __ATOMIC_RELAXED);
}

/* A lock that needs no initialisation and nothing from the C library. */
typedef struct {
  volatile int held;
} WeavecRtLock;

static inline void weavecRtLock(WeavecRtLock *lock) {
  unsigned spins = 0;
  while (__atomic_exchange_n(&lock->held, 1, __ATOMIC_ACQUIRE) != 0) {
    while (__atomic_load_n(&lock->held, __ATOMIC_RELAXED) != 0) {
      if (++spins < 64) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
      } else {
        extern int sched_yield(void);
        (void)sched_yield();
      }
    }
  }
}

static inline void weavecRtUnlock(WeavecRtLock *lock) {
  __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE);
}

#endif /* WEAVEC_RUNTIME_WEAVEC_RT_H */
