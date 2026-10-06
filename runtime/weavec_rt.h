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
#include <string.h>

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

/* What a lookup of an arena pointer needs; `bytes` is 0 until the arena
 * exists, so every pointer is outside it. The guards the backend expands
 * (RFC 0034, section 1.4) read `shadow` and `mask` at fixed offsets. The
 * slot words of a region are readable as soon as the arena exists; a slot
 * the region has not reached reads as never allocated. */
struct __weavec_rt_heap_t {
  uintptr_t base;
  /* classes << shift. */
  uintptr_t bytes;
  /* RFC 0034, section 2.1: one byte per 16-byte granule of the address
   * space, at shadow + ((address >> 4) & mask). 0: no live arena object's
   * (outside the arena, never allocated, free, quarantined); 1-16: the
   * first k bytes are a live arena object's; 0xFE: a live object's slot
   * after the object. `mask` is 0 when the shadow could not be reserved,
   * and until it is: every guard then reads the byte at `shadow`, which is
   * 0, and asks the runtime. */
  uintptr_t shadow;
  uintptr_t mask;
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

/* Shadow bytes (RFC 0034, section 2.1, as amended). A granule's byte is
 * 0 when no live tracked object has bytes in it (an untracked address, or
 * a dead or never-allocated arena slot: `base` and `bytes` tell them
 * apart); 1-16 when the first k bytes are a live heap object's (arena or
 * huge block); `Tail` after a live heap object's last byte, in its slot.
 * A stack or global object that starts on a granule (the compiler aligns
 * the ones it registers) is encoded exactly: its last granule reads
 * `ObjectLast + k` (k, 1-16, of its bytes are there), each granule r before
 * it a run byte: `ObjectRun + r` for r up to 48, and above it
 * `ObjectRun + 48 + c`, which says r is at least 2^(c + 4) (c from 1 to 15,
 * the largest r below 2^20 granules). A granule d before another is of the
 * same object when d is at most the earlier one's least r; the end of an
 * object is a logarithmic number of such jumps away. An object that does not start on a granule makes its granules, and
 * the one after it, `Mixed`, which the runtime looks up. `HugeDead` marks a
 * released huge block. */
enum {
  WeavecRtShadowDead = 0x00,
  WeavecRtShadowWhole = 0x10,
  WeavecRtShadowObjectLast = 0x40,
  WeavecRtShadowObjectRun = 0x80,
  WeavecRtShadowRunLimit = 63,
  /* The untracked granule after an exactly encoded object that ends on a
   * granule: an address one past an object belongs to it. */
  WeavecRtShadowOnePast = 0xFB,
  WeavecRtShadowMixed = 0xFC,
  WeavecRtShadowHugeDead = 0xFD,
  WeavecRtShadowTail = 0xFE
};

/*===-- The shadow (RFC 0034, section 2) -----------------------------------===*/

static inline unsigned char *weavecRtShadowOf(uintptr_t address) {
  return (unsigned char *)(__weavec_rt_heap.shadow +
                           ((address >> 4) & __weavec_rt_heap.mask));
}

static inline int weavecRtShadowReady(void) {
  return __atomic_load_n(&__weavec_rt_heap.mask, __ATOMIC_ACQUIRE) != 0;
}

/* Whether `v` is a stack or global object's exact encoding, and its r. */
static inline int weavecRtIsObjectByte(unsigned char v) {
  return (v > WeavecRtShadowObjectLast && v <= WeavecRtShadowObjectLast + 16) ||
         (v > WeavecRtShadowObjectRun &&
          v <= WeavecRtShadowObjectRun + WeavecRtShadowRunLimit);
}
/* The least r a run byte says (0 for a last granule). */
static inline uintptr_t weavecRtObjectRun(unsigned char v) {
  if (v <= WeavecRtShadowObjectRun)
    return 0;
  if (v <= WeavecRtShadowObjectRun + 48)
    return v - WeavecRtShadowObjectRun;
  return (uintptr_t)1 << (v - WeavecRtShadowObjectRun - 48 + 4);
}
/* The run byte of a granule r granules before its object's last. */
static inline unsigned char weavecRtRunByte(uintptr_t r) {
  unsigned c = 1;
  if (r <= 48)
    return (unsigned char)(WeavecRtShadowObjectRun + r);
  while (c < 15 && ((uintptr_t)1 << (c + 5)) <= r)
    ++c;
  return (unsigned char)(WeavecRtShadowObjectRun + 48 + c);
}

/* A live heap object of `size` bytes at `base` (16-aligned): its whole
 * granules, then the granule its end falls in, which is a partial granule
 * or the slot's tail. */
static inline void weavecRtShadowLive(uintptr_t base, size_t size) {
  unsigned char *shadow = weavecRtShadowOf(base);
  const size_t whole = size >> 4;
  const unsigned rest = (unsigned)(size & 15);
  if (!weavecRtShadowReady())
    return;
  if (whole != 0)
    memset(shadow, WeavecRtShadowWhole, whole);
  shadow[whole] = rest != 0 ? (unsigned char)rest : WeavecRtShadowTail;
}

/* Sets what `weavecRtShadowLive(base, size)` marked to `value`. */
static inline void weavecRtShadowFill(uintptr_t base, size_t size,
                                      unsigned char value) {
  if (weavecRtShadowReady())
    memset(weavecRtShadowOf(base), value, (size >> 4) + 1);
}

/* A live stack or global object of `size` bytes at `base`: exactly when it
 * starts on a granule, else `Mixed` over its granules and the next. When no
 * storage the runtime does not know can follow it (`known`: a global's
 * padding, a frame with no unnamed storage), an untracked granule after it
 * says that an address one past it belongs to it; otherwise it has the
 * whole of a last granule it fills only in part, since unknown storage may
 * share it. The
 * granule after it may start a live exactly encoded neighbour, whose bytes
 * stay (its leave walks them); one past the end of this one then reads as
 * the neighbour's, as with two exact objects. */
static inline void weavecRtShadowObject(uintptr_t base, size_t size,
                                        int known) {
  uintptr_t g;
  if (!weavecRtShadowReady() || size == 0)
    return;
  if ((base & 15) == 0) {
    const uintptr_t n = (size + 15) >> 4;
    /* (A range the mask does not wrap is contiguous in the shadow: the
     * granules of one run byte are written at once.) */
    if (weavecRtShadowOf(base + ((n - 1) << 4)) >= weavecRtShadowOf(base)) {
      unsigned char *at = weavecRtShadowOf(base);
      uintptr_t r = n - 1;
      while (r > 48) {
        const unsigned char v = weavecRtRunByte(r);
        const uintptr_t low = v == weavecRtRunByte(49) ? 49 : weavecRtObjectRun(v);
        memset(at, v, r - low + 1);
        at += r - low + 1;
        r = low - 1;
      }
      for (; r > 0; --r)
        *at++ = weavecRtRunByte(r);
    } else {
      for (g = 0; g + 1 < n; ++g)
        *weavecRtShadowOf(base + (g << 4)) = weavecRtRunByte(n - 1 - g);
    }
    /* A last granule it shares with a live mixed object stays mixed. */
    if ((size & 15) == 0 ||
        *weavecRtShadowOf(base + ((n - 1) << 4)) != WeavecRtShadowMixed)
      *weavecRtShadowOf(base + ((n - 1) << 4)) =
          (unsigned char)(WeavecRtShadowObjectLast +
                          (known ? size - ((n - 1) << 4) : 16));
    if (known && (size & 15) == 0 && *weavecRtShadowOf(base + (n << 4)) == 0)
      *weavecRtShadowOf(base + (n << 4)) = WeavecRtShadowOnePast;
    return;
  }
  for (g = base >> 4; g <= (base + size) >> 4; ++g) {
    unsigned char *byte = weavecRtShadowOf(g << 4);
    if (g << 4 < base + size || !weavecRtIsObjectByte(*byte))
      *byte = WeavecRtShadowMixed;
  }
}

/* The object at `base` is gone: an exactly encoded one reads untracked; of
 * a mixed one, the granules it had to itself (the runtime looks the others
 * up). */
static inline void weavecRtShadowForget(uintptr_t base, size_t size) {
  if (!weavecRtShadowReady() || size == 0)
    return;
  if ((base & 15) != 0) {
    uintptr_t g;
    for (g = (base + 15) >> 4; (g + 1) << 4 <= base + size; ++g)
      *weavecRtShadowOf(g << 4) = WeavecRtShadowDead;
    return;
  }
  memset(weavecRtShadowOf(base), WeavecRtShadowDead, size >> 4);
  if ((size & 15) != 0 &&
      *weavecRtShadowOf(base + ((size >> 4) << 4)) != WeavecRtShadowMixed)
    *weavecRtShadowOf(base + ((size >> 4) << 4)) = WeavecRtShadowDead;
  if (*weavecRtShadowOf(base + (((size + 15) >> 4) << 4)) ==
      WeavecRtShadowOnePast)
    *weavecRtShadowOf(base + (((size + 15) >> 4) << 4)) = WeavecRtShadowDead;
}

/* Zeroes `bytes` of the shadow at `start`: whole pages by mapping them
 * afresh, so that a thread's 64 MiB stack costs pages, not bytes. */
void weavecRtShadowZero(unsigned char *start, size_t bytes);

/* Clears the shadow of [low, high) (16-aligned outward). */
static inline void weavecRtShadowClear(uintptr_t low, uintptr_t high) {
  uintptr_t g;
  if (!weavecRtShadowReady() || high <= low)
    return;
  /* (A range the mask does not wrap is contiguous in the shadow.) */
  if (((high + 15) >> 4) - (low >> 4) > 65536 &&
      weavecRtShadowOf(high - 1) > weavecRtShadowOf(low)) {
    weavecRtShadowZero(weavecRtShadowOf(low),
                       (size_t)(((high + 15) >> 4) - (low >> 4)));
    return;
  }
  for (g = low >> 4; g < (high + 15) >> 4; ++g)
    *weavecRtShadowOf(g << 4) = WeavecRtShadowDead;
}

/* The end (one past the last byte) of the exactly encoded stack or global
 * object whose granule holds `address`, found by jumping forward: 0 when
 * the granule is not one. A run of r below 63 ends r granules on; one of
 * 63 at least 63 on. */
static inline uintptr_t weavecRtShadowObjectEnd(uintptr_t address) {
  uintptr_t g = address >> 4;
  unsigned char v;
  if (!weavecRtShadowReady())
    return 0;
  v = *weavecRtShadowOf(g << 4);
  while (v > WeavecRtShadowObjectLast + 16) {
    if (!weavecRtIsObjectByte(v))
      return 0;
    g += weavecRtObjectRun(v);
    v = *weavecRtShadowOf(g << 4);
  }
  if (!weavecRtIsObjectByte(v))
    return 0;
  return (g << 4) + (v - WeavecRtShadowObjectLast);
}

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

/* Guards: 0 when the guard passes, non-zero when it fails. */
WEAVEC_RT_API int __weavec_rt_object(const void *p, long long index,
                                     unsigned long long step,
                                     unsigned long long offset,
                                     unsigned long long width);
WEAVEC_RT_API int __weavec_rt_string(const char *p);
/* The length of the string at `p`, read inside its own object: the maximum
 * when the object is dead or holds no terminator. */
WEAVEC_RT_API unsigned long long __weavec_rt_strlen(const char *p);
/* RFC 0034, section 5.2: the bytes from `p` to the end of the live tracked
 * object it points into; 0 in a dead one, the maximum in none. */
WEAVEC_RT_API unsigned long long __weavec_rt_room(const void *p);
WEAVEC_RT_API int __weavec_rt_live(const void *p);
/* RFC 0034, section 2.5: the slow path of a guard the backend expanded,
 * and of every guard an object built without the backend pass calls. `kind`
 * is a WeavecRtGuard value, with WeavecRtGuardOverflow when the access's
 * address overflowed and WeavecRtGuardProven for verify mode's monitors of
 * proven facets. Returns when the guard passes; otherwise traps, or, with a
 * `site` (report mode), reports it and returns. */
enum {
  WeavecRtGuardObject = 0,
  WeavecRtGuardLive = 1,
  WeavecRtGuardKindMask = 3,
  WeavecRtGuardOverflow = 4,
  WeavecRtGuardProven = 8
};
struct __weavec_rt_site {
  const char *file;
  unsigned line;
  unsigned column;
};
WEAVEC_RT_API void __weavec_rt_guard(const void *from, const void *at,
                                     unsigned long long width, unsigned kind,
                                     const struct __weavec_rt_site *site);
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
  void (*guard)(const void *, const void *, unsigned long long, unsigned,
                const struct __weavec_rt_site *);
  void *(*stackEnter)(void *, size_t, void *, int);
  void (*stackLeave)(void *, void *);
  void (*stackRewind)(void *);
  void (*globalsAdd)(const void *const *, const void *const *);
  void (*report)(const char *, const char *, unsigned, unsigned);
  void (*fatal)(const char *, const void *, const char *);
  unsigned long long (*room)(const void *);
};
WEAVEC_RT_API extern const struct __weavec_rt_dispatch __weavec_rt_dispatch;

#if defined(__APPLE__)
/* The owner's table when this copy forwards to it; null when this copy is
 * the owner (or while it is finding out, on the thread doing so). The state
 * is resolved once, so every later entry point tests one global inline. */
enum { WeavecRtOwnerResolved = 2 };
extern unsigned weavecRtOwnerState;
extern const struct __weavec_rt_dispatch *weavecRtOwner;
const struct __weavec_rt_dispatch *weavecRtResolveOwner(void);
static inline const struct __weavec_rt_dispatch *weavecRtForward(void) {
  if (__builtin_expect(__atomic_load_n(&weavecRtOwnerState, __ATOMIC_ACQUIRE) ==
                           WeavecRtOwnerResolved,
                       1))
    return weavecRtOwner;
  return weavecRtResolveOwner();
}
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
  WeavecRtStatSlowGuards,
  WeavecRtStatStackEnters,
  WeavecRtStatCount
};
extern unsigned long long weavecRtStats[WeavecRtStatCount];
static inline void weavecRtCount(int stat) { ++weavecRtStats[stat]; }

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
