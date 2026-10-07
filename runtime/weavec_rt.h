/*===- weavec_rt.h - The WeaveC runtime's internal interface ------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, section 5. Shared by the runtime's own sources and its tests;
|* not installed. Compiled code reaches the runtime through the entry points
|* the guard pass calls (lib/Frontend/GuardPass*.cpp declares the same
|* ones), the shadow descriptor it reads inline, and the standard allocation
|* functions libweavec_alloc.a defines.
|*
\*===----------------------------------------------------------------------===*/

#ifndef WEAVEC_RUNTIME_WEAVEC_RT_H
#define WEAVEC_RUNTIME_WEAVEC_RT_H

#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Every entry point has default visibility: on ELF the first definition the
 * dynamic linker finds then serves every image. */
#define WEAVEC_RT_API __attribute__((visibility("default")))

/* The guards' slow paths keep every register of their caller, so that an
 * inline guard costs the code around it no spills and the register
 * allocator no live-range splits; the guard pass calls them so on AArch64
 * and x86-64. Clang builds the runtime. */
#if defined(__aarch64__) || defined(__x86_64__)
#define WEAVEC_RT_SLOW __attribute__((preserve_all))
#else
#define WEAVEC_RT_SLOW
#endif

/*===-- The shadow (section 2.2) ------------------------------------------===*/

/* One byte per 16-byte granule of a window of the address space, at
 * base + ((address >> 4) & mask). The guard pass reads `base` and `mask` at
 * offsets 0 and 8 once per function. Until the shadow is reserved, and when
 * it cannot be, `mask` is 0 and `base` points to zero bytes the frame code
 * may scribble on: every guard then passes, because the slow path knows. */
struct __weavec_rt_shadow_t {
  uintptr_t base;
  uintptr_t mask;
  /* Addresses at or above 1 << bits alias lower ones (section 2.2). */
  unsigned bits;
};

WEAVEC_RT_API extern struct __weavec_rt_shadow_t __weavec_rt_shadow;

/* Shadow values. 0: the granule's 16 bytes are addressable; 1-15: its first
 * k bytes are; the rest name why bytes are not. */
enum {
  WeavecRtShadowAddressable = 0x00,
  WeavecRtShadowGranule = 16,
  WeavecRtShadowStackLeft = 0xF1,
  WeavecRtShadowStackMid = 0xF2,
  WeavecRtShadowStackRight = 0xF3,
  WeavecRtShadowStackDynamic = 0xF4,
  WeavecRtShadowStackScope = 0xF8,
  WeavecRtShadowGlobal = 0xF9,
  WeavecRtShadowHeapTail = 0xFA,
  WeavecRtShadowHeapFreed = 0xFD,
  /* The lowest WeavecRtNullPage bytes: an access there is through null. */
  WeavecRtShadowNull = 0xFE
};
enum { WeavecRtNullPage = 65536 };

static inline int weavecRtShadowReady(void) {
  return __atomic_load_n(&__weavec_rt_shadow.mask, __ATOMIC_ACQUIRE) != 0;
}

/* Whether the shadow describes `address` itself rather than an alias. */
static inline int weavecRtInWindow(uintptr_t address) {
  return __weavec_rt_shadow.bits >= 64 ||
         (address >> __weavec_rt_shadow.bits) == 0;
}

static inline unsigned char *weavecRtShadowOf(uintptr_t address) {
  return (unsigned char *)(__weavec_rt_shadow.base +
                           ((address >> 4) & __weavec_rt_shadow.mask));
}

/* Whether the n bytes at `address` are addressable, decided from one
 * eight-byte load of the shadow (the inline guard's chunk check); 0 when
 * that cannot tell (more than 113 bytes, or a poisoned byte), for the full
 * check to decide. */
static inline int weavecRtQuickOk(uintptr_t address, uint64_t n) {
  uint64_t last;
  unsigned shift;
  uint64_t chunk;
  unsigned char value;
  if (n == 0)
    return 1;
  if (n > 113 || !weavecRtShadowReady() || !weavecRtInWindow(address))
    return 0;
  last = (address & 15) + n - 1;
  shift = (unsigned)(last >> 4) * 8;
  /* (The shadow has room for an eight-byte load past its last byte.) */
  __builtin_memcpy(&chunk, weavecRtShadowOf(address), sizeof chunk);
  if (shift != 0 && (chunk & (((uint64_t)1 << shift) - 1)) != 0)
    return 0;
  value = (unsigned char)(chunk >> shift);
  return value == WeavecRtShadowAddressable ||
         (value < WeavecRtShadowGranule && (last & 15) < value);
}

/* Writes `value` over the shadow of [base, base + size), 16-aligned base,
 * whole granules (the last one partial counts whole). */
void weavecRtShadowSet(uintptr_t base, size_t size, unsigned char value);

/* Marks [base, base + size) addressable and the rest of its last granule
 * not, with `tail`; base is 16-aligned. */
void weavecRtShadowObject(uintptr_t base, size_t size, unsigned char tail);

/* Clears the shadow of [low, high), rounded outwards to granules; whole
 * pages of it by mapping them afresh. */
void weavecRtShadowClear(uintptr_t low, uintptr_t high);

/* The calling thread's stack top (its highest address), or 0. */
uintptr_t weavecRtStackTop(void);

/*===-- The arena (sections 5.1) -------------------------------------------===*/

enum {
  /* 16, 32, ..., 128, then four classes per doubling up to 2^30. */
  WeavecRtClassCount = 100,
  WeavecRtSmallClasses = 8,
  WeavecRtMaxClassShift = 30,
  /* A dead slot at least this large gives its pages back. */
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
 * exists, so every pointer is outside it. */
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

/* Slot states: the low two bits of a slot word. */
enum {
  WeavecRtNever = 0,
  WeavecRtLive = 1,
  WeavecRtFree = 2,
  WeavecRtDead = 3
};

WEAVEC_RT_API void *__weavec_rt_alloc(size_t size, size_t alignment);
WEAVEC_RT_API void __weavec_rt_free(void *p);
WEAVEC_RT_API void *__weavec_rt_realloc(void *p, size_t size);
/* The requested size of a live block; of any other pointer, what the next
 * allocator says, or 0. */
WEAVEC_RT_API size_t __weavec_rt_size(const void *p);

/* What the allocator knows of an address, for reports: whether it is in a
 * heap slot or huge block, and that object's start, size and state. */
struct weavecRtHeapObject {
  int found;
  int live;
  uintptr_t base;
  size_t size;
};
struct weavecRtHeapObject weavecRtHeapObjectAt(uintptr_t address);

/*===-- Guards (section 5.2) -----------------------------------------------===*/

/* The constant record the guard pass emits per guarded site. */
enum {
  WeavecRtSiteWrite = 1,
  /* Report mode: report once and return instead of trapping. */
  WeavecRtSiteReport = 2,
  /* Verify mode's monitor of a guard section 6 removed. */
  WeavecRtSiteProven = 4,
  /* A string the call accepts as null (`system(NULL)`). */
  WeavecRtSiteNullOk = 8
};
struct __weavec_rt_site {
  const char *file;
  unsigned line;
  unsigned column;
  unsigned flags;
};

/* The slow path of an inline guard of `width` bytes at `address`: returns
 * when the bytes are addressable; else traps, or reports and returns. */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_guard(uintptr_t address, uint64_t width,
                  const struct __weavec_rt_site *site);
/* A guard of a base that may be null (section 2.4). */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_null(uintptr_t address, const struct __weavec_rt_site *site);
/* A range guard; `n` may be 0. */
WEAVEC_RT_API void __weavec_rt_range(uintptr_t address, uint64_t n,
                                     const struct __weavec_rt_site *site);
/* Whether the range is addressable, without a report (section 6.4). */
WEAVEC_RT_API int __weavec_rt_range_ok(uintptr_t address, uint64_t n);
/* The length of the string at `s`, checking that its bytes and terminator
 * are addressable; `max` bounds the scan (the bytes read are then at most
 * `max`). */
WEAVEC_RT_API uint64_t __weavec_rt_strlen(const char *s, uint64_t max,
                                          const struct __weavec_rt_site *site);

/* A copy whose operands overlap (section 2.5). */
WEAVEC_RT_API void __weavec_rt_overlap(uintptr_t destination, uintptr_t source,
                                       uint64_t n,
                                       const struct __weavec_rt_site *site);

/* Checked library calls (section 2.5). */
WEAVEC_RT_API char *__weavec_rt_strcpy(char *d, const char *s,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_stpcpy(char *d, const char *s,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_strcat(char *d, const char *s,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_sprintf(const struct __weavec_rt_site *site,
                                      char *d, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vsprintf(char *d, const char *format,
                                       va_list arguments,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_snprintf(const struct __weavec_rt_site *site,
                                       char *d, size_t n, const char *format,
                                       ...);
WEAVEC_RT_API int __weavec_rt_vsnprintf(char *d, size_t n, const char *format,
                                        va_list arguments,
                                        const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_gets(char *d,
                                     const struct __weavec_rt_site *site);
/* The scanf family checks, after the call, what each conversion that ran
 * wrote. */
WEAVEC_RT_API int __weavec_rt_sscanf(const struct __weavec_rt_site *site,
                                     const char *s, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vsscanf(const char *s, const char *format,
                                      va_list arguments,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_fscanf(const struct __weavec_rt_site *site,
                                     FILE *stream, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vfscanf(FILE *stream, const char *format,
                                      va_list arguments,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_scanf(const struct __weavec_rt_site *site,
                                    const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vscanf(const char *format, va_list arguments,
                                     const struct __weavec_rt_site *site);
/* The searches and comparisons check the bytes they read, up to where
 * they stop. */
WEAVEC_RT_API void *__weavec_rt_memchr(const void *s, int c, size_t n,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_strchr(const char *s, int c,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strcmp(const char *a, const char *b,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strncmp(const char *a, const char *b, size_t n,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strcasecmp(const char *a, const char *b,
                                         const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strncasecmp(const char *a, const char *b,
                                          size_t n,
                                          const struct __weavec_rt_site *site);
/* The mappings clear the shadow of what they map and unmap. */
WEAVEC_RT_API void *__weavec_rt_mmap(void *address, size_t length,
                                     int protection, int flags, int fd,
                                     long long offset,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_munmap(void *address, size_t length,
                                     const struct __weavec_rt_site *site);

/* Frames (section 3). */
WEAVEC_RT_API void __weavec_rt_unpoison_stack(void);
/* A dynamic stack object of `size` bytes at `address` (32-aligned), followed
 * by its redzone up to the next 32-byte boundary and 32 bytes more. */
WEAVEC_RT_API void __weavec_rt_alloca_poison(uintptr_t address, uint64_t size);
/* Clears the shadow of [low, high): the dynamic objects a stack restore or a
 * return frees. */
WEAVEC_RT_API void __weavec_rt_alloca_unpoison(uintptr_t low, uintptr_t high);

/* Globals (section 4): `n` records {address, size, size with redzone}. */
struct __weavec_rt_global {
  uintptr_t address;
  uint64_t size;
  uint64_t padded;
};
WEAVEC_RT_API void
__weavec_rt_globals_register(const struct __weavec_rt_global *globals,
                             uint64_t n);
WEAVEC_RT_API void
__weavec_rt_globals_unregister(const struct __weavec_rt_global *globals,
                               uint64_t n);

/*===-- Reports (section 5.3) ----------------------------------------------===*/

/* Reports a failed guard of `kind` (a report kind's name) and traps, unless
 * the site asks for a report, which is printed once per site. */
void weavecRtFail(const char *kind, uintptr_t address, uint64_t width,
                  const struct __weavec_rt_site *site);
/* Called by a failure right before it traps, so that the trap ends the
 * program even where the program blocked or caught SIGTRAP and SIGILL. */
WEAVEC_RT_API void __weavec_rt_trapping(void);
/* Prints `weavec: <what> of <pointer>: <why>` and traps. */
WEAVEC_RT_API void __weavec_rt_fatal(const char *what, const void *p,
                                     const char *why)
    __attribute__((noreturn));

/*===-- One runtime per process --------------------------------------------===*/

/* Every image linked by weavec-cc carries a copy of the runtime. On Darwin
 * each copy would have its own arena and shadow, so one of them, the one
 * `dlsym(RTLD_DEFAULT, "__weavec_rt_dispatch")` finds (the same for every
 * image), owns the process: the others forward their allocation entry
 * points to it and copy its shadow descriptor, which their inline guards
 * read. On ELF the dynamic linker already binds every image to the first
 * definition, and nothing forwards. */
struct __weavec_rt_dispatch {
  unsigned magic;
  unsigned version;
  const struct __weavec_rt_heap_t *heap;
  const struct __weavec_rt_shadow_t *shadow;
  int (*initialise)(void);
  void *(*alloc)(size_t, size_t);
  void (*release)(void *);
  void *(*realloc)(void *, size_t);
  size_t (*size)(const void *);
  struct weavecRtHeapObject (*object)(uintptr_t);
  void (*fatal)(const char *, const void *, const char *);
};
WEAVEC_RT_API extern const struct __weavec_rt_dispatch __weavec_rt_dispatch;

#if defined(__APPLE__)
/* The owner's table when this copy forwards to it; null when this copy is
 * the owner (or while it is finding out, on the thread doing so). */
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

/* Reserves the shadow and this copy's arena on first use; 0 when there is
 * no room for the arena. */
int weavecRtInitialise(void);

/* WEAVEC_RT_STATS=1: what the runtime did, printed when the program exits. */
enum {
  WeavecRtStatAllocations,
  WeavecRtStatReleases,
  WeavecRtStatRecycled,
  WeavecRtStatHuge,
  WeavecRtStatSlowGuards,
  WeavecRtStatRanges,
  WeavecRtStatStrings,
  WeavecRtStatStackUnpoisons,
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
