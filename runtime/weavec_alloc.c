/*===- weavec_alloc.c - The size-class arena allocator -------------*- C
-*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0032, section 2. Every heap object of the image lives in a slot of a
|* size-class region of one reserved arena, so the object a pointer points
|* into, its requested size and whether it is live follow from the pointer
|* by arithmetic and one load:
|*
|*   region = (p - arena) >> shift
|*   slot   = offset in the region / class size   (a multiplication)
|*   word   = metadata[region][slot]              (size << 2 | state)
|*
|* RFC 0035, section 5.1: the shadow (one byte per 16-byte granule, see
|* weavec_rt.h) says which bytes are addressable. An allocation marks its
|* object addressable and the rest of its slot not (0xFA); a release marks
|* the whole slot released (0xFD). The allocator writes it whenever a slot
|* word changes.
|*
|* A request takes a slot of the smallest class strictly larger than it, so
|* at least one byte after every object belongs to no object. Slot words,
|* the free lists and the quarantine queues live in the metadata, never in
|* freed memory. RFC 0036 section 3: a released slot waits in its class's
|* quarantine until a sweep (weavec_sweep.c) finds no pointer to it; only
|* then does it join the free list. Requests no class can hold are mapped on
|* their own and kept in a table; a released one stays reserved, without
|* access, until a sweep finds no pointer to it either.
|*
|* The allocator needs nothing from the C library's allocator, so it can
|* replace it: it maps its own memory and takes its own locks.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <dlfcn.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* The bytes every guard reads until the shadow exists (`mask` is 0): zero,
 * so every guard passes or asks the runtime, which knows. Wide enough for
 * the widest shadow load a guard makes. */
static unsigned char scratchShadow[64] __attribute__((aligned(16)));

struct __weavec_rt_shadow_t __weavec_rt_shadow = {(uintptr_t)scratchShadow, 0,
                                                  64};

/* Until the arena exists `bytes` is 0, so every pointer is outside it. */
struct __weavec_rt_heap_t __weavec_rt_heap = {0, 0, 0, 0, 0, 0, {{0, 0, 0, 0}}};

/*===-- State
 * ---------------------------------------------------------------===*/

/* What one region needs besides the public table; guarded by `lock`. Each
 * on its own cache line, so that threads in different classes do not share
 * one (RFC 0037 section 2.2). */
typedef struct {
  WeavecRtLock lock;
  /* Slots handed out so far from the region's start. */
  uint32_t bump;
  /* Slot index + 1 of the first free slot; 0 for none. */
  uint32_t freeHead;
  /* The quarantine queue: slot index + 1 of its oldest and newest slot. */
  uint32_t deadHead;
  uint32_t deadTail;
  /* The sweep's marks, one bit per slot, mapped on the first sweep. */
  uint64_t *marks;
  /* Bytes of the region, and of its metadata, that are accessible. */
  uintptr_t committed;
  uintptr_t metaCommitted;
} __attribute__((aligned(128))) RegionState;

static RegionState regionState[WeavecRtRegionCount];

/* RFC 0037 section 2.3: a class's regions, its own first and then the
 * spares it took, linked by `regionNext` (NoRegion ends the list); the
 * region allocations try first; the bytes released into the class since
 * the last sweep. */
enum { NoRegion = 0xFF };
static uint8_t regionNext[WeavecRtRegionCount];
static uint8_t regionClass[WeavecRtRegionCount];
static uint8_t classActive[WeavecRtClassCount];
static size_t classGained[WeavecRtClassCount];
static WeavecRtLock spareLock;

/* RFC 0037 section 2.1: each thread's caches of free slots to allocate and
 * of released slots on their way to the quarantine, per small class. A
 * slot address's low bit says it was used before (its bytes are not known
 * to be zero). A released slot in a cache is quarantined: its word says
 * released, with the value `CachedDead`, so a second release of it is a
 * double free and a sweep never recycles it. */
enum {
  CachedClasses = 40,
  CacheSlots = 32,
  CacheBytes = 32 * 1024,
  /* A thread's changes to the shared counters, folded in at this size. */
  FoldBytes = 256 * 1024,
  CachedDead = (1 << 30) - 1
};

typedef struct {
  uint32_t count;
  uint32_t limit;
  uint64_t slots[CacheSlots];
} SlotCache;

/* A cached slot: its region and index, the low bit set for a slot used
 * before, and the top bit set so that the sweep, which reads the cache's
 * memory, never takes one for a pointer. */
static inline uint64_t slotEntry(unsigned region, uint32_t slot, int used) {
  return (uint64_t)1 << 63 | (uint64_t)region << 32 | (uint64_t)slot << 1 |
         (uint64_t)(used != 0);
}
static inline unsigned entryRegion(uint64_t entry) {
  return (unsigned)(entry >> 32) & 0xFF;
}
static inline uint32_t entrySlot(uint64_t entry) {
  return (uint32_t)(entry >> 1) & 0x7FFFFFFF;
}

typedef struct ThreadCache {
  struct ThreadCache *next;
  SlotCache alloc[CachedClasses];
  SlotCache release[CachedClasses];
  /* What this thread added to the live heap since it last folded it into
   * the shared count. */
  intptr_t liveDelta;
  /* The thread's calls into the allocator in progress, and the allocator
   * locks it holds: a signal handler that allocates while the thread is
   * inside the allocator must not touch the cache, sweep, or wait for a lock
   * the thread holds (RFC 0037 amendment 11). Releases it could not make
   * wait here until the outer call returns. */
  unsigned depth;
  unsigned locks;
  unsigned deferredCount;
  void *deferred[16];
} ThreadCache;

/* A thread whose cache has gone (its key's destructor ran) uses none. */
#define CacheGone ((ThreadCache *)1)

static pthread_key_t cacheKey;
static int cacheKeyReady;
static unsigned cachedClasses;
static ThreadCache *caches;
static WeavecRtLock cacheListLock;

/* The calling thread's value of the cache's key: on Darwin read from its
 * TSD array directly, as libpthread's inline accessor does (the call costs
 * as much as the rest of a small allocation's fast path). */
static inline void *cacheValue(void) {
#if defined(__APPLE__) && defined(__aarch64__)
  uintptr_t tsd;
  __asm__("mrs %0, tpidrro_el0" : "=r"(tsd));
  return ((void *const *)(tsd & ~(uintptr_t)7))[cacheKey];
#elif defined(__APPLE__) && defined(__x86_64__)
  void *value;
  __asm__("movq %%gs:(,%1,8), %0" : "=r"(value) : "r"((uintptr_t)cacheKey));
  return value;
#else
  return pthread_getspecific(cacheKey);
#endif
}

/* The calling thread's cache if it has one; none is made. */
static inline ThreadCache *ownCache(void) {
  ThreadCache *cache;
  if (!__atomic_load_n(&cacheKeyReady, __ATOMIC_RELAXED))
    return NULL;
  cache = (ThreadCache *)cacheValue();
  return cache != CacheGone ? cache : NULL;
}

/* An allocator lock, counted in the thread's cache (see `locks`). */
static inline void take(WeavecRtLock *lock) {
  ThreadCache *self = ownCache();
  if (self != NULL)
    ++self->locks;
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  weavecRtLock(lock);
}

static inline void give(WeavecRtLock *lock) {
  ThreadCache *self = ownCache();
  weavecRtUnlock(lock);
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  if (self != NULL)
    --self->locks;
}

/* A block mapped on its own (section 2.3). */
typedef struct {
  uintptr_t base;
  size_t size;
  uintptr_t map;
  size_t mapBytes;
  /* 0 while live; else the order of its release. */
  unsigned long long dead;
  /* A pointer to the released block was found by the running sweep. */
  int marked;
} HugeBlock;

static WeavecRtLock initLock;
static WeavecRtLock hugeLock;
static HugeBlock *hugeBlocks;
static size_t hugeCount;
static size_t hugeCapacity;
/* The lowest and highest address a huge block has had: a quick rejection. */
static uintptr_t hugeLow = ~(uintptr_t)0;
static uintptr_t hugeHigh;
/* Released huge blocks stay reserved, without access, until a sweep finds
 * no pointer to them: a stale pointer then finds a dead block, and nothing
 * else can be mapped there. */
static size_t hugeDeadCount;
/* Their bytes, which count toward the quarantine's for when a sweep runs
 * (RFC 0036 amendment 10): they hold address space and, on Linux, a
 * mapping each. */
static size_t hugeDeadBytes;
static unsigned long long hugeReleases;

static size_t pageBytes;
/* RFC 0036 section 3.3: the configured quarantine (`Q`), the bytes it
 * holds, what the last sweep left in it (pointers still reach them), and
 * the bytes of live blocks. */
static size_t quarantineBudget = (size_t)64 << 20;
static size_t quarantineBytes;
static size_t retainedBytes;
static size_t liveBytes;
/* WEAVEC_RT_STATS=1: the most the live heap and the quarantine held. */
static size_t peakLive;
static size_t peakQuarantine;
/* The pacing of section 3.3: when the last sweep ended and how long it
 * took, and the released bytes at which the time is looked at next. */
static uint64_t lastSweepEnd;
static uint64_t lastSweepTook;
static size_t nextPacingCheck;

enum { DataChunk = 1 << 20, MetaChunk = 64 * 1024 };

/*===-- Classes
 * -------------------------------------------------------------===*/

/* The class whose slots are the smallest that are strictly larger than
 * `size`; the class count when there is none. */
static unsigned classFor(size_t size) {
  const size_t need = size + 1;
  if (need <= 128)
    return (unsigned)((need - 1) / 16);
  if (need > ((size_t)1 << WeavecRtMaxClassShift))
    return WeavecRtClassCount;
  {
    /* Sizes are m << k with m in 5..8 and k >= 5. */
    const unsigned k =
        (unsigned)(63 - __builtin_clzll((unsigned long long)(need - 1))) - 2;
    const unsigned m = (unsigned)((need - 1) >> k) + 1;
    return WeavecRtSmallClasses + 4 * (k - 5) + (m - 5);
  }
}

/* The power-of-two class of exactly `bytes` (a power of two, at least 16). */
static unsigned powerClass(size_t bytes) {
  return classFor(bytes - 1);
}

static void buildClasses(unsigned shift) {
  unsigned index;
  unsigned count = 0;
  for (index = 0; index < WeavecRtClassCount; ++index) {
    struct __weavec_rt_class *entry = &__weavec_rt_heap.table[index];
    unsigned m;
    unsigned k;
    if (index < WeavecRtSmallClasses) {
      m = index + 1;
      k = 4;
    } else {
      m = 5 + (index - WeavecRtSmallClasses) % 4;
      k = 5 + (index - WeavecRtSmallClasses) / 4;
    }
    /* Normalize so that m is odd or small: the divisor of the slot index. */
    while (m % 2 == 0 && m > 1) {
      m /= 2;
      ++k;
    }
    entry->size = (uint32_t)m << k;
    entry->shift = k;
    entry->magic = m == 1 ? 0 : (uint32_t)((((uint64_t)1 << 32) + m - 1) / m);
    /* A class larger than a quarter of its region is not created. */
    if ((uint64_t)entry->size > ((uint64_t)1 << (shift - 2)))
      break;
    entry->capacity = (uint32_t)(((uint64_t)1 << shift) / entry->size);
    ++count;
  }
  __weavec_rt_heap.classes = count;
}

static inline uint32_t *slotWords(unsigned region) {
  return (uint32_t *)(__weavec_rt_heap.meta +
                      ((uintptr_t)region << (__weavec_rt_heap.shift - 2)));
}

static inline uintptr_t regionStart(unsigned region) {
  return __weavec_rt_heap.base + ((uintptr_t)region << __weavec_rt_heap.shift);
}

/*===-- The shadow (RFC 0034, section 2)
 * -----------------------------------===*/

/* Whether a core dump includes [p, p + bytes). The shadow and the arena's
 * reservations are terabytes a dump would otherwise walk page by page (and,
 * piped to a crash handler, write out as zeros): a trapping program would
 * not end. What the arena has committed is dumped. */
static void dumpable(void *p, size_t bytes, int dump) {
#if defined(MADV_DONTDUMP)
  (void)madvise(p, bytes, dump ? MADV_DODUMP : MADV_DONTDUMP);
#else
  (void)p;
  (void)bytes;
  (void)dump;
#endif
}

/* RFC 0035, section 2.2: the window the shadow covers. On Linux the user
 * address space, whose width the stack's address gives (it lies near the
 * top); on Darwin 2^42 bytes, because the kernel's cost of a reservation
 * grows with its size (about 1.7 ms per TiB to map and unmap, per process),
 * and user mappings, the arena's included, lie below 2^41. */
static unsigned windowBits(void) {
#if defined(__APPLE__)
  return 42;
#else
  const uintptr_t stack = (uintptr_t)__builtin_frame_address(0);
  unsigned bits = 64 - (unsigned)__builtin_clzll((unsigned long long)stack);
  if (bits < 39)
    bits = 39;
  return bits;
#endif
}

/* Reserves the shadow of the window, without backing: pages no one wrote
 * read 0, addressable. */
static void reserveShadow(void) {
  const unsigned bits = windowBits();
  const uintptr_t bytes = (uintptr_t)1 << (bits - 4);
  /* (Room for the widest load past the last byte.) */
  void *map = mmap(NULL, bytes + 64, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED) {
    static const char Message[] =
        "weavec: warning: the shadow memory could not be reserved; memory "
        "accesses are not checked\n";
    const long written = (long)write(2, Message, sizeof Message - 1);
    (void)written;
    return;
  }
  dumpable(map, bytes + 64, 0);
  weavecRtInternal((uintptr_t)map, (uintptr_t)map + bytes + 64);
  __weavec_rt_shadow.bits = bits;
  __atomic_store_n(&__weavec_rt_shadow.base, (uintptr_t)map, __ATOMIC_RELEASE);
  /* An access near null fails its guard even when the optimizer moved
   * the access itself to the other arm of the pointer's select. */
  memset(map, WeavecRtShadowNull, WeavecRtNullPage >> 4);
  /* Published last: until then every guard passes. */
  __atomic_store_n(&__weavec_rt_shadow.mask, bytes - 1, __ATOMIC_RELEASE);
}

/* Writes `n` shadow bytes of `value`: a small object's few bytes with
 * stores (each release and allocation writes them), larger runs with
 * memset. */
static inline void fill(unsigned char *at, unsigned char value, size_t n) {
  if (n > 16) {
    memset(at, value, n);
    return;
  }
  if (n >= 8) {
    const uint64_t pattern = 0x0101010101010101ULL * value;
    memcpy(at, &pattern, sizeof pattern);
    memcpy(at + n - 8, &pattern, sizeof pattern);
  } else if (n >= 4) {
    const uint32_t pattern = 0x01010101U * value;
    memcpy(at, &pattern, sizeof pattern);
    memcpy(at + n - 4, &pattern, sizeof pattern);
  } else if (n != 0) {
    at[0] = value;
    at[n - 1] = value;
    at[n / 2] = value;
  }
}

void weavecRtShadowSet(uintptr_t base, size_t size, unsigned char value) {
  if (!weavecRtShadowReady() || size == 0 || !weavecRtInWindow(base))
    return;
  fill(weavecRtShadowOf(base), value, (size + 15) >> 4);
}

void weavecRtShadowObject(uintptr_t base, size_t size, unsigned char tail,
                          unsigned char color) {
  const size_t whole = size >> 4;
  const unsigned rest = (unsigned)(size & 15);
  if (!weavecRtShadowReady() || !weavecRtInWindow(base))
    return;
  /* A huge block's shadow is mapped afresh rather than written, so that
   * the pages of a block nothing touches cost nothing: it is uncolored
   * (RFC 0037 section 7.2). */
  if (whole >= 4 * pageBytes) {
    color = 0;
    weavecRtShadowClear(base, base + (whole << 4));
  } else if (whole != 0) {
    fill(weavecRtShadowOf(base), color, whole);
  }
  /* A partial last granule says how many of its bytes are the object's; an
   * empty object's granule is all tail. */
  if (rest != 0)
    *weavecRtShadowOf(base + (whole << 4)) =
        weavecRtShadowPartialOf(color, rest);
  else if (size == 0)
    *weavecRtShadowOf(base) = tail;
}

void weavecRtShadowClear(uintptr_t low, uintptr_t high) {
  unsigned char *start;
  size_t bytes;
  uintptr_t first;
  uintptr_t last;
  if (!weavecRtShadowReady() || high <= low || !weavecRtInWindow(low) ||
      !weavecRtInWindow(high - 1))
    return;
  start = weavecRtShadowOf(low);
  bytes = (size_t)(((high + 15) >> 4) - (low >> 4));
  first = ((uintptr_t)start + pageBytes - 1) & ~(uintptr_t)(pageBytes - 1);
  last = ((uintptr_t)start + bytes) & ~(uintptr_t)(pageBytes - 1);
  /* Whole pages are mapped afresh, so that a thread's 64 MiB stack costs
   * pages, not bytes. */
  if (bytes < 4 * pageBytes || last <= first ||
      mmap((void *)first, last - first, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1,
           0) == MAP_FAILED) {
    memset(start, WeavecRtShadowAddressable, bytes);
    return;
  }
  dumpable((void *)first, last - first, 0);
  memset(start, WeavecRtShadowAddressable, first - (uintptr_t)start);
  memset((void *)last, WeavecRtShadowAddressable,
         (uintptr_t)start + bytes - last);
}

/* The most of a slot's tail, and of slots not handed out yet, that is
 * poisoned: a large slot's shadow is not all written, and an access that
 * far past an object's end is beyond any redzone ASan keeps. */
enum { TailLimit = 64 * 1024 };

/* Zeroes a recycled block. The whole pages of a large one are mapped afresh
 * rather than written, so that the pages a release gave back stay given
 * back until the program touches them. */
static void zeroFill(uintptr_t base, size_t size) {
  const uintptr_t first = (base + pageBytes - 1) & ~(uintptr_t)(pageBytes - 1);
  const uintptr_t last = (base + size) & ~(uintptr_t)(pageBytes - 1);
  if (size < 16 * pageBytes || last <= first ||
      mmap((void *)first, last - first, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED) {
    memset((void *)base, 0, size);
    return;
  }
  memset((void *)base, 0, first - base);
  memset((void *)last, 0, base + size - last);
}

/* A live object of `size` bytes at the start of `slot` bytes: the object
 * addressable, the rest of the slot (up to TailLimit bytes of it) not. */
static void shadowLive(uintptr_t base, size_t size, size_t slot) {
  const size_t used = (size + 15) & ~(size_t)15;
  weavecRtShadowObject(base, size, WeavecRtShadowHeapTail,
                       weavecRtColorOfHeap(base));
  if (slot > used)
    weavecRtShadowSet(base + used,
                      slot - used < TailLimit ? slot - used : TailLimit,
                      WeavecRtShadowHeapTail);
}

/* A released slot of `slot` bytes. */
static void shadowDead(uintptr_t base, size_t slot) {
  weavecRtShadowSet(base, slot, WeavecRtShadowHeapFreed);
}

static inline uint32_t loadWord(const uint32_t *word) {
  return __atomic_load_n(word, __ATOMIC_RELAXED);
}

static inline void storeWord(uint32_t *word, uint32_t value) {
  __atomic_store_n(word, value, __ATOMIC_RELAXED);
}

/*===-- Initialization
 * ------------------------------------------------------===*/

static void forkPrepare(void);
static void forkParent(void);
static void forkChild(void);
static void dropCache(void *value);
#if defined(__APPLE__)
static void registerZone(void);
static void promoteZone(void);
#else
extern int pthread_atfork(void (*)(void), void (*)(void), void (*)(void))
    __attribute__((weak));
#endif

/* Reserves the arena with regions of 2^shift bytes; 0 when the address
 * space is not available. */
static int reserve(unsigned shift) {
  uintptr_t data;
  uintptr_t meta;
  uintptr_t total;
  uintptr_t align;
  uintptr_t start;
  void *map;
  unsigned region;
  buildClasses(shift);
  data = (uintptr_t)WeavecRtRegionCount << shift;
  meta = (uintptr_t)WeavecRtRegionCount << (shift - 2);
  align = (uintptr_t)1 << shift;
  /* One alignment more than the regions need: a granule before the first
   * region is the arena's too (its guard). */
  total = data + meta + 2 * align;
  map = mmap(NULL, total, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED)
    return 0;
  dumpable(map, total, 0);
  /* The sweep scans the arena's live slots, not the reservation. */
  weavecRtInternal((uintptr_t)map, (uintptr_t)map + total);
  /* Regions start at multiples of their size, so that the slots of a
   * power-of-two class are aligned to it. */
  start = ((uintptr_t)map + WeavecRtShadowGranule + align - 1) & ~(align - 1);
  /* A guard reads a slot word without asking how far its region has got:
   * the words are readable from the start, and zero (never allocated)
   * until written. */
  if (mprotect((void *)(start + data), meta, PROT_READ) != 0) {
    (void)munmap(map, total);
    return 0;
  }
  /* Each class starts with the region of its index; the rest are spares,
   * whose one slot until a class takes one is never allocated. */
  for (region = 0; region < WeavecRtRegionCount; ++region) {
    regionNext[region] = NoRegion;
    regionClass[region] =
        region < __weavec_rt_heap.classes ? (uint8_t)region : NoRegion;
    if (region >= __weavec_rt_heap.classes) {
      __weavec_rt_heap.table[region].size = (uint32_t)1 << (shift - 1);
      __weavec_rt_heap.table[region].shift = shift;
      __weavec_rt_heap.table[region].magic = 0;
      __weavec_rt_heap.table[region].capacity = 0;
    }
  }
  for (region = 0; region < __weavec_rt_heap.classes; ++region)
    classActive[region] = (uint8_t)region;
  __weavec_rt_heap.regions = __weavec_rt_heap.classes;
  __weavec_rt_heap.shift = shift;
  __weavec_rt_heap.meta = start + data;
  __atomic_store_n(&__weavec_rt_heap.base, start, __ATOMIC_RELEASE);
  /* Published last: until then every pointer is outside the arena. */
  __atomic_store_n(&__weavec_rt_heap.bytes, data, __ATOMIC_RELEASE);
  return 1;
}

int weavecRtInitialize(void) {
  static int tried; /* once: a failed reservation is not tried again */
  int ok;
  if (__atomic_load_n(&__weavec_rt_heap.bytes, __ATOMIC_ACQUIRE) != 0)
    return 1;
  weavecRtLock(&initLock);
  ok = __weavec_rt_heap.bytes != 0;
  if (!ok && !tried) {
    const long page = sysconf(_SC_PAGESIZE);
    tried = 1;
    pageBytes = page > 0 ? (size_t)page : 4096;
    reserveShadow();
    /* Regions of 2 GiB (1 GiB when that much cannot be reserved): a full
     * region's class takes a spare, and the reservation costs start-up time
     * with its size. */
    ok = reserve(31) || reserve(30);
    if (ok) {
      unsigned index;
      cachedClasses = 0;
      for (index = 0;
           index < __weavec_rt_heap.classes && index < CachedClasses &&
           __weavec_rt_heap.table[index].size <= CacheBytes;
           ++index)
        cachedClasses = index + 1;
      if (pthread_key_create(&cacheKey, dropCache) == 0)
        __atomic_store_n(&cacheKeyReady, 1, __ATOMIC_RELEASE);
    }
    if (ok && weavecRtShadowReady() &&
        !weavecRtInWindow(__weavec_rt_heap.base + __weavec_rt_heap.bytes - 1)) {
      static const char Message[] =
          "weavec: warning: the heap lies outside the shadow's window; heap "
          "accesses are not checked\n";
      const long written = (long)write(2, Message, sizeof Message - 1);
      (void)written;
    }
    if (ok) {
#if defined(__APPLE__)
      registerZone();
      promoteZone();
#else
      if (pthread_atfork != NULL)
        (void)pthread_atfork(forkPrepare, forkParent, forkChild);
#endif
    }
  }
  weavecRtUnlock(&initLock);
  return ok;
}

/* Before the program's own code: a function reads the shadow descriptor
 * once on entry, so `main` would otherwise keep the empty one, and pass
 * every guard, when its first allocation reserves the shadow. The library
 * is every guarded image's dependency, so this runs before their
 * constructors; on Darwin it also promotes the zone, so that the C
 * library's allocations in the program's first statements are in it. */
__attribute__((constructor(101))) static void initializeEarly(void) {
  (void)weavecRtInitialize();
}

/*===-- Slots
 * ---------------------------------------------------------------===*/

/* Makes [committed, end) of a range accessible, in chunks; `limit` is the
 * range's size. */
static int commit(uintptr_t start, uintptr_t *committed, uintptr_t end,
                  uintptr_t chunk, uintptr_t limit) {
  uintptr_t target;
  if (end <= *committed)
    return 1;
  if (chunk < pageBytes)
    chunk = pageBytes;
  target = (end + chunk - 1) / chunk * chunk;
  if (target > limit)
    target = limit;
  if (mprotect((void *)(start + *committed), target - *committed,
               PROT_READ | PROT_WRITE) != 0)
    return 0;
  dumpable((void *)(start + *committed), target - *committed, 1);
  *committed = target;
  return 1;
}

/* The slot of an arena address: its region, index and base. */
typedef struct {
  unsigned region;
  uint32_t slot;
  uintptr_t base;
} SlotRef;

static inline int inArena(uintptr_t address) {
  return address - __weavec_rt_heap.base < __weavec_rt_heap.bytes;
}

static inline SlotRef slotOf(uintptr_t address) {
  const uintptr_t offset = address - __weavec_rt_heap.base;
  const unsigned region = (unsigned)(offset >> __weavec_rt_heap.shift);
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[region];
  const uint64_t in = offset & (((uintptr_t)1 << __weavec_rt_heap.shift) - 1);
  const uint64_t units = in >> entry->shift;
  SlotRef ref;
  ref.region = region;
  ref.slot = entry->magic == 0 ? (uint32_t)units
                               : (uint32_t)((units * entry->magic) >> 32);
  ref.base = regionStart(region) + (uintptr_t)ref.slot * entry->size;
  return ref;
}

/* RFC 0037 section 7.2: a slot's color, from its index and its region's:
 * neighbors in a region differ, and so do any two slots fewer than 111
 * apart. */
unsigned char weavecRtColorOfHeap(uintptr_t address) {
  SlotRef ref;
  if (!inArena(address))
    return 0;
  ref = slotOf(address);
  return weavecRtColor((uint64_t)ref.slot + (uint64_t)ref.region * 41);
}

/* The word of a slot; "never allocated" for one the region has not
 * reached. */
static inline uint32_t wordOf(SlotRef ref) {
  return loadWord(&slotWords(ref.region)[ref.slot]);
}

/* Takes a slot of `region` off its free list, or a fresh one; false when
 * the region is exhausted or memory is. The class's lock is held.
 * `recycled` says the slot was used before, so its bytes are not known to
 * be zero. */
static int popSlot(unsigned region, uint32_t *taken, int *recycled) {
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[region];
  RegionState *state = &regionState[region];
  uint32_t *words = slotWords(region);
  uint32_t slot;
  *recycled = 0;
  if (state->freeHead != 0) {
    slot = state->freeHead - 1;
    state->freeHead = weavecRtWordValue(loadWord(&words[slot]));
    *recycled = 1;
  } else {
    const uintptr_t limit = (uintptr_t)1 << __weavec_rt_heap.shift;
    const uintptr_t before = state->committed;
    slot = state->bump;
    if (slot >= entry->capacity ||
        !commit(regionStart(region), &state->committed,
                ((uintptr_t)slot + 1) * entry->size, DataChunk, limit) ||
        !commit((uintptr_t)words, &state->metaCommitted,
                ((uintptr_t)slot + 1) * sizeof(uint32_t), MetaChunk,
                limit >> 2))
      return 0;
    /* RFC 0035 section 5.1: slots the region has not handed out yet are
     * no object's, so an index that jumps past an object's slot into them
     * fails; each is written again when it is allocated. */
    if (state->committed > before)
      weavecRtShadowSet(regionStart(region) + before,
                        entry->size <= TailLimit
                            ? (size_t)(state->committed - before)
                            : TailLimit,
                        WeavecRtShadowHeapTail);
    /* The granule before the region (the previous region's last, or the
     * arena's guard) too: a read just before the first slot fails as an
     * overflow instead of reaching memory nothing has committed. A slot
     * allocated there later keeps its last byte poisoned (its slack). */
    if (before == 0)
      weavecRtShadowSet(regionStart(region) - WeavecRtShadowGranule,
                        WeavecRtShadowGranule, WeavecRtShadowHeapTail);
    __atomic_store_n(&state->bump, slot + 1, __ATOMIC_RELAXED);
  }
  *taken = slot;
  return 1;
}

/* Pops up to `want` slots of `region` into `slots` (slotEntry); how
 * many. */
static unsigned popRegion(unsigned region, uint64_t *slots, unsigned want) {
  RegionState *state = &regionState[region];
  unsigned got = 0;
  take(&state->lock);
  while (got < want) {
    uint32_t slot;
    int recycled;
    if (!popSlot(region, &slot, &recycled))
      break;
    slots[got++] = slotEntry(region, slot, recycled);
  }
  give(&state->lock);
  return got;
}

/* Gives class `c` a spare region and makes it the one its allocations try
 * first; false when none is left (RFC 0037 section 2.3). */
static int takeSpare(unsigned c) {
  unsigned region;
  unsigned last = c;
  take(&spareLock);
  region = __weavec_rt_heap.regions;
  if (region >= WeavecRtRegionCount) {
    give(&spareLock);
    return 0;
  }
  __weavec_rt_heap.table[region] = __weavec_rt_heap.table[c];
  regionClass[region] = (uint8_t)c;
  while (regionNext[last] != NoRegion)
    last = regionNext[last];
  __atomic_store_n(&__weavec_rt_heap.regions, region + 1, __ATOMIC_RELEASE);
  __atomic_store_n(&regionNext[last], (uint8_t)region, __ATOMIC_RELEASE);
  __atomic_store_n(&classActive[c], (uint8_t)region, __ATOMIC_RELEASE);
  give(&spareLock);
  return 1;
}

/* Pops up to `want` (at least one) slots of class `c`: from the region its
 * allocations try first, then from another of its regions; when every one
 * is full, after a sweep if a quarter of a region was released into the
 * class since the last one, else from a spare (RFC 0037 section 2.3). How
 * many; 0 when memory is exhausted. */
static unsigned popClass(unsigned c, uint64_t *slots, unsigned want) {
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[c];
  int swept = 0;
  for (;;) {
    unsigned region = __atomic_load_n(&classActive[c], __ATOMIC_ACQUIRE);
    unsigned got = popRegion(region, slots, want);
    if (got != 0)
      return got;
    for (region = c; region != NoRegion;
         region = __atomic_load_n(&regionNext[region], __ATOMIC_ACQUIRE)) {
      got = popRegion(region, slots, want);
      if (got != 0) {
        __atomic_store_n(&classActive[c], (uint8_t)region, __ATOMIC_RELEASE);
        return got;
      }
    }
    if (!swept && __atomic_load_n(&classGained[c], __ATOMIC_RELAXED) >=
                      (size_t)entry->capacity * entry->size / 4) {
      swept = 1;
      weavecRtSweep();
      continue;
    }
    if (takeSpare(c))
      continue;
    /* No spare is left: the last chance is what a sweep recycles. */
    if (swept)
      return 0;
    swept = 1;
    weavecRtSweep();
  }
}

/* Adds `bytes` (negative: takes them) to the live heap's count. */
static void addLive(intptr_t bytes) {
  const size_t live =
      __atomic_add_fetch(&liveBytes, (size_t)bytes, __ATOMIC_RELAXED);
  if ((intptr_t)live > (intptr_t)peakLive)
    peakLive = live;
}

/* The live heap's bytes; a thread's folded release may briefly arrive
 * before another's allocation. */
static size_t liveNow(void) {
  const intptr_t live = (intptr_t)__atomic_load_n(&liveBytes, __ATOMIC_RELAXED);
  return live > 0 ? (size_t)live : 0;
}

/* Makes the slot `taken` (slotEntry) live with an object of `size`
 * bytes. */
static void *makeLive(uint64_t taken, size_t size, size_t slotBytes) {
  const unsigned region = entryRegion(taken);
  const uint32_t slot = entrySlot(taken);
  const uintptr_t base = regionStart(region) + (uintptr_t)slot * slotBytes;
  storeWord(&slotWords(region)[slot],
            ((uint32_t)size << WeavecRtWordShift) | WeavecRtLive);
  if ((taken & 1) != 0 && size != 0)
    zeroFill(base, size);
  shadowLive(base, size, slotBytes);
  return (void *)base;
}

/* A slot of class `c` for an object of `size` bytes, live, without a
 * thread cache; null when memory is exhausted. */
static void *takeSlot(unsigned c, size_t size) {
  const size_t slotBytes = __weavec_rt_heap.table[c].size;
  uint64_t taken;
  if (popClass(c, &taken, 1) == 0)
    return NULL;
  addLive((intptr_t)slotBytes);
  return makeLive(taken, size, slotBytes);
}

/*===-- Thread caches (RFC 0037 section 2.1)
 * --------------------------------===*/

static unsigned cacheLimit(unsigned c) {
  const size_t fit = CacheBytes / __weavec_rt_heap.table[c].size;
  return fit == 0 ? 1 : fit < CacheSlots ? (unsigned)fit : CacheSlots;
}

/* The calling thread's cache, made on its first use; null when it has none
 * (it is exiting, or the cache cannot be mapped). */
static ThreadCache *makeCache(void);

static inline ThreadCache *threadCache(void) {
  ThreadCache *cache;
  /* Set once, before any thread but the initializing one can allocate. */
  if (__builtin_expect(!__atomic_load_n(&cacheKeyReady, __ATOMIC_RELAXED), 0))
    return NULL;
  cache = (ThreadCache *)cacheValue();
  if (__builtin_expect(cache != NULL && cache != CacheGone, 1))
    return cache;
  return cache == CacheGone ? NULL : makeCache();
}

/* A new cache for the calling thread. */
static __attribute__((noinline)) ThreadCache *makeCache(void) {
  ThreadCache *cache;
  unsigned c;
  cache = (ThreadCache *)mmap(NULL, sizeof(ThreadCache), PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ((void *)cache == MAP_FAILED)
    return NULL;
  for (c = 0; c < cachedClasses; ++c)
    cache->alloc[c].limit = cache->release[c].limit = cacheLimit(c);
  if (pthread_setspecific(cacheKey, cache) != 0) {
    (void)munmap(cache, sizeof(ThreadCache));
    return NULL;
  }
  take(&cacheListLock);
  cache->next = caches;
  caches = cache;
  give(&cacheListLock);
  return cache;
}

/* Folds a thread's change of the live heap into the shared count. */
static void foldLive(ThreadCache *cache) {
  if (cache->liveDelta != 0)
    addLive(cache->liveDelta);
  cache->liveDelta = 0;
}

/* Moves class `c`'s released slots from the cache to their regions'
 * quarantines, holding a region's lock across a run of its slots. With
 * `check`, a slot that is not one (a cache a fork copied mid-push) is
 * left. */
static void flushReleases(ThreadCache *cache, unsigned c, int check) {
  SlotCache *released = &cache->release[c];
  const size_t slotBytes = __weavec_rt_heap.table[c].size;
  const uint32_t claimed =
      ((uint32_t)CachedDead << WeavecRtWordShift) | WeavecRtDead;
  RegionState *held = NULL;
  size_t bytes = 0;
  unsigned i;
  for (i = 0; i < released->count; ++i) {
    const unsigned region = entryRegion(released->slots[i]);
    const uint32_t slot = entrySlot(released->slots[i]);
    RegionState *state = &regionState[region];
    uint32_t *words = slotWords(region);
    if (check &&
        (region >= __weavec_rt_heap.regions || regionClass[region] != c ||
         slot >= state->bump || loadWord(&words[slot]) != claimed))
      continue;
    if (state != held) {
      if (held != NULL)
        give(&held->lock);
      take(&state->lock);
      held = state;
    }
    storeWord(&words[slot], WeavecRtDead);
    if (state->deadTail != 0)
      storeWord(&words[state->deadTail - 1],
                ((slot + 1) << WeavecRtWordShift) | WeavecRtDead);
    else
      state->deadHead = slot + 1;
    state->deadTail = slot + 1;
    bytes += slotBytes;
  }
  if (held != NULL)
    give(&held->lock);
  released->count = 0;
  if (bytes != 0) {
    const size_t total =
        __atomic_add_fetch(&quarantineBytes, bytes, __ATOMIC_RELAXED);
    (void)__atomic_add_fetch(&classGained[c], bytes, __ATOMIC_RELAXED);
    if (total > peakQuarantine)
      peakQuarantine = total;
  }
}

/* Gives class `c`'s cached free slots back to their regions' free lists. */
static void returnSlots(ThreadCache *cache, unsigned c, int check) {
  SlotCache *available = &cache->alloc[c];
  unsigned i;
  for (i = 0; i < available->count; ++i) {
    const unsigned region = entryRegion(available->slots[i]);
    const uint32_t slot = entrySlot(available->slots[i]);
    RegionState *state = &regionState[region];
    uint32_t *words = slotWords(region);
    if (check &&
        (region >= __weavec_rt_heap.regions || regionClass[region] != c ||
         slot >= state->bump ||
         (loadWord(&words[slot]) & WeavecRtWordState) == WeavecRtLive ||
         (loadWord(&words[slot]) & WeavecRtWordState) == WeavecRtDead))
      continue;
    take(&state->lock);
    storeWord(&words[slot],
              (state->freeHead << WeavecRtWordShift) | WeavecRtFree);
    state->freeHead = slot + 1;
    give(&state->lock);
  }
  available->count = 0;
}

/* Empties a cache into the shared lists and counts. */
static void drainCache(ThreadCache *cache, int check) {
  unsigned c;
  for (c = 0; c < cachedClasses; ++c) {
    flushReleases(cache, c, check);
    returnSlots(cache, c, check);
  }
  foldLive(cache);
}

/* A thread's exit: its cache goes, and the thread uses none from then on
 * (a destructor that runs after this one may still allocate). */
static void dropCache(void *value) {
  ThreadCache *cache = (ThreadCache *)value;
  (void)pthread_setspecific(cacheKey, CacheGone);
  if (cache == NULL || cache == CacheGone)
    return;
  drainCache(cache, 0);
  take(&cacheListLock);
  {
    ThreadCache **at = &caches;
    while (*at != NULL && *at != cache)
      at = &(*at)->next;
    if (*at != NULL)
      *at = cache->next;
  }
  give(&cacheListLock);
  (void)munmap(cache, sizeof(ThreadCache));
}

void weavecRtFlushThread(void) {
  ThreadCache *cache;
  if (!__atomic_load_n(&cacheKeyReady, __ATOMIC_ACQUIRE))
    return;
  cache = (ThreadCache *)cacheValue();
  if (cache != NULL && cache != CacheGone)
    drainCache(cache, 0);
}

static int overBudget(void);

/* An allocation of class `c` from the cache, refilled when empty. */
static void *cachedAlloc(ThreadCache *cache, unsigned c, size_t size) {
  SlotCache *available = &cache->alloc[c];
  const size_t slotBytes = __weavec_rt_heap.table[c].size;
  if (available->count == 0) {
    unsigned i;
    available->count = popClass(c, available->slots, available->limit);
    if (available->count == 0)
      return NULL;
    /* Taken from the end: the lowest address last in, so that slots are
     * handed out in address order (RFC 0036 amendment 4). */
    for (i = 0; i < available->count / 2; ++i) {
      const uint64_t swap = available->slots[i];
      available->slots[i] = available->slots[available->count - 1 - i];
      available->slots[available->count - 1 - i] = swap;
    }
  }
  cache->liveDelta += (intptr_t)slotBytes;
  if (cache->liveDelta >= FoldBytes)
    foldLive(cache);
  return makeLive(available->slots[--available->count], size, slotBytes);
}

/*===-- The quarantine (section 2.4)
 * ----------------------------------------===*/

/* Read in a constructor, never by `free`: the C library frees with its
 * environment lock held (`unsetenv`), and `getenv` takes it again. */
__attribute__((constructor)) static void readQuarantineBudget(void) {
  const char *text = getenv("WEAVEC_RT_QUARANTINE");
  if (text != NULL && *text >= '0' && *text <= '9')
    quarantineBudget = (size_t)strtoull(text, NULL, 10);
}

static uint64_t nanoseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* RFC 0036 section 3.3: whether the quarantine should be swept. `gained`,
 * the bytes released since the last sweep, is past the hard budget
 * max(2 Q, live / 2); or past the soft budget max(Q, live / 4) with the
 * last sweep at least eight of its durations ago, so that sweeps take at
 * most about a ninth of the run time. The clock is read once per sixteenth
 * of the soft budget released. */
static int overBudget(void) {
  const size_t held = __atomic_load_n(&quarantineBytes, __ATOMIC_RELAXED) +
                      __atomic_load_n(&hugeDeadBytes, __ATOMIC_RELAXED);
  const size_t kept = __atomic_load_n(&retainedBytes, __ATOMIC_RELAXED);
  const size_t live = liveNow();
  const size_t gained = held > kept ? held - kept : 0;
  const size_t soft = quarantineBudget > live / 4 ? quarantineBudget : live / 4;
  const size_t hard =
      2 * quarantineBudget > live / 2 ? 2 * quarantineBudget : live / 2;
  if (gained < soft ||
      gained < __atomic_load_n(&nextPacingCheck, __ATOMIC_RELAXED))
    return 0;
  if (gained >= hard ||
      nanoseconds() - __atomic_load_n(&lastSweepEnd, __ATOMIC_RELAXED) >=
          8 * __atomic_load_n(&lastSweepTook, __ATOMIC_RELAXED))
    return 1;
  __atomic_store_n(&nextPacingCheck, gained + soft / 16, __ATOMIC_RELAXED);
  return 0;
}

void weavecRtPeaks(size_t *live, size_t *quarantine) {
  *live = peakLive;
  *quarantine = peakQuarantine;
}

/* After a sweep that could not mark (a thread that never answered, no
 * memory for the marks), which recycled nothing, the next waits for another
 * hard budget of releases (RFC 0038, amendments 13 and 26): a thread that
 * never answers costs a second per hard budget. */
void weavecRtSwept(uint64_t ended, uint64_t took, int marked) {
  const size_t held = __atomic_load_n(&quarantineBytes, __ATOMIC_RELAXED) +
                      __atomic_load_n(&hugeDeadBytes, __ATOMIC_RELAXED);
  const size_t kept = __atomic_load_n(&retainedBytes, __ATOMIC_RELAXED);
  const size_t live = liveNow();
  const size_t hard =
      2 * quarantineBudget > live / 2 ? 2 * quarantineBudget : live / 2;
  __atomic_store_n(&lastSweepEnd, ended, __ATOMIC_RELAXED);
  __atomic_store_n(&lastSweepTook, took, __ATOMIC_RELAXED);
  __atomic_store_n(&nextPacingCheck,
                   marked ? 0 : (held > kept ? held - kept : 0) + hard,
                   __ATOMIC_RELAXED);
}

/* Gives the whole pages inside a dead slot back to the system, at once: a
 * released slot waits for a sweep while fresh slots are taken, so its pages
 * must not stay resident meanwhile (Darwin's MADV_FREE leaves them resident
 * until the system needs memory; a fresh mapping drops them). Its shadow
 * stays: it says the slot is released (a page given back reads 0). */
static void decommit(uintptr_t base, size_t bytes) {
  const uintptr_t first = (base + pageBytes - 1) & ~(uintptr_t)(pageBytes - 1);
  const uintptr_t last = (base + bytes) & ~(uintptr_t)(pageBytes - 1);
  if (last <= first)
    return;
#if defined(__APPLE__)
  if (mmap((void *)first, last - first, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
    (void)madvise((void *)first, last - first, MADV_FREE);
#else
  (void)madvise((void *)first, last - first, MADV_DONTNEED);
#endif
}

/* Takes the live slot `ref`, whose word was `word`, for its release: its
 * word says released, with the value `CachedDead` (a sweep leaves it be
 * until it is queued), so a second release of it, even one racing this
 * one, is a double free. */
static void claim(SlotRef ref, uint32_t word, void *p) {
  uint32_t *at = &slotWords(ref.region)[ref.slot];
  if (!__atomic_compare_exchange_n(
          at, &word, ((uint32_t)CachedDead << WeavecRtWordShift) | WeavecRtDead,
          0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
    __weavec_rt_fatal("invalid release", p, "the block was already released");
}

/* Queues the claimed slot `ref` in its region's quarantine; sweeps when
 * the quarantine is over budget. */
static void retire(SlotRef ref) {
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[ref.region];
  RegionState *state = &regionState[ref.region];
  uint32_t *words = slotWords(ref.region);
  /* Before the slot is queued: a sweep may recycle it once it is. */
  shadowDead(ref.base, entry->size);
  if (entry->size >= WeavecRtDecommitBytes)
    decommit(ref.base, entry->size);
  take(&state->lock);
  storeWord(&words[ref.slot], WeavecRtDead);
  if (state->deadTail != 0)
    storeWord(&words[state->deadTail - 1],
              ((ref.slot + 1) << WeavecRtWordShift) | WeavecRtDead);
  else
    state->deadHead = ref.slot + 1;
  state->deadTail = ref.slot + 1;
  give(&state->lock);
  {
    const size_t held =
        __atomic_add_fetch(&quarantineBytes, entry->size, __ATOMIC_RELAXED);
    if (held > peakQuarantine)
      peakQuarantine = held;
  }
  (void)__atomic_add_fetch(&classGained[regionClass[ref.region]], entry->size,
                           __ATOMIC_RELAXED);
  addLive(-(intptr_t)entry->size);
  if (overBudget())
    weavecRtSweep();
}

/* A release through the thread's cache: the slot waits there, claimed,
 * until the cache's class fills and moves to the quarantine. */
static void cachedRelease(ThreadCache *cache, unsigned c, SlotRef ref) {
  SlotCache *released = &cache->release[c];
  const size_t slotBytes = __weavec_rt_heap.table[c].size;
  shadowDead(ref.base, slotBytes);
  released->slots[released->count++] = slotEntry(ref.region, ref.slot, 1);
  cache->liveDelta -= (intptr_t)slotBytes;
  if (cache->liveDelta <= -(intptr_t)FoldBytes)
    foldLive(cache);
  if (released->count == released->limit) {
    flushReleases(cache, c, 0);
    if (overBudget())
      weavecRtSweep();
  }
}

/*===-- Huge blocks
 * ---------------------------------------------------------===*/

/* The index of the last block whose base is at most `address`, or -1. The
 * lock is held. */
static long hugeIndex(uintptr_t address) {
  size_t low = 0;
  size_t high = hugeCount;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (hugeBlocks[mid].map <= address)
      low = mid + 1;
    else
      high = mid;
  }
  return (long)low - 1;
}

static void *hugeAlloc(size_t size, size_t alignment) {
  const size_t page = pageBytes;
  size_t bytes;
  size_t mapBytes;
  uintptr_t map;
  uintptr_t base;
  size_t at;
  if (alignment < page)
    alignment = page;
  /* RFC 0037 section 2.3: a block the shadow's window has no room for (more
   * than a quarter of it) fails, as the system allocator fails one the
   * address space has no room for. */
  if (size > (~(size_t)0 >> 1) - 2 * alignment - 2 * page ||
      (weavecRtShadowReady() && __weavec_rt_shadow.bits < 64 &&
       size > ((size_t)1 << (__weavec_rt_shadow.bits - 2)))) {
    errno = ENOMEM;
    return NULL;
  }
  bytes = (size + page - 1) / page * page;
  if (bytes == 0)
    bytes = page;
  /* Room to align, and one inaccessible page after the block. */
  mapBytes = bytes + page + (alignment > page ? alignment : 0);
  map = (uintptr_t)mmap(NULL, mapBytes, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ((void *)map == MAP_FAILED) {
    errno = ENOMEM;
    return NULL;
  }
  base = (map + alignment - 1) & ~(uintptr_t)(alignment - 1);
  (void)mprotect((void *)(base + bytes), page, PROT_NONE);

  take(&hugeLock);
  if (hugeCount == hugeCapacity) {
    const size_t capacity = hugeCapacity != 0 ? hugeCapacity * 2 : 256;
    HugeBlock *grown = (HugeBlock *)mmap(NULL, capacity * sizeof(HugeBlock),
                                         PROT_READ | PROT_WRITE,
                                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if ((void *)grown == MAP_FAILED) {
      give(&hugeLock);
      (void)munmap((void *)map, mapBytes);
      errno = ENOMEM;
      return NULL;
    }
    if (hugeCount != 0)
      memcpy(grown, hugeBlocks, hugeCount * sizeof(HugeBlock));
    weavecRtInternal((uintptr_t)grown,
                     (uintptr_t)grown + capacity * sizeof(HugeBlock));
    if (hugeBlocks != NULL) {
      weavecRtInternalDrop((uintptr_t)hugeBlocks);
      (void)munmap(hugeBlocks, hugeCapacity * sizeof(HugeBlock));
    }
    hugeBlocks = grown;
    hugeCapacity = capacity;
  }
  at = (size_t)(hugeIndex(map) + 1);
  memmove(&hugeBlocks[at + 1], &hugeBlocks[at],
          (hugeCount - at) * sizeof(HugeBlock));
  hugeBlocks[at].base = base;
  hugeBlocks[at].size = size;
  hugeBlocks[at].map = map;
  hugeBlocks[at].mapBytes = mapBytes;
  hugeBlocks[at].dead = 0;
  hugeBlocks[at].marked = 0;
  ++hugeCount;
  shadowLive(base, size, bytes);
  if (map < __atomic_load_n(&hugeLow, __ATOMIC_RELAXED))
    __atomic_store_n(&hugeLow, map, __ATOMIC_RELAXED);
  if (map + mapBytes > __atomic_load_n(&hugeHigh, __ATOMIC_RELAXED))
    __atomic_store_n(&hugeHigh, map + mapBytes, __ATOMIC_RELAXED);
  give(&hugeLock);
  return (void *)base;
}

static inline int maybeHuge(uintptr_t address) {
  return address >= __atomic_load_n(&hugeLow, __ATOMIC_RELAXED) &&
         address < __atomic_load_n(&hugeHigh, __ATOMIC_RELAXED);
}

/* The huge block that holds `address`, live or dead. */
static int hugeFind(uintptr_t address, HugeBlock *out) {
  int found = 0;
  long index;
  if (!maybeHuge(address))
    return 0;
  take(&hugeLock);
  index = hugeIndex(address);
  if (index >= 0 &&
      address < hugeBlocks[index].map + hugeBlocks[index].mapBytes) {
    *out = hugeBlocks[index];
    found = 1;
  }
  give(&hugeLock);
  return found;
}

/* Releases the live huge block that starts at `address`; false when there
 * is none. */
static int hugeFree(uintptr_t address) {
  long index;
  int ok = 0;
  if (!maybeHuge(address))
    return 0;
  take(&hugeLock);
  index = hugeIndex(address);
  if (index >= 0 && hugeBlocks[index].base == address &&
      hugeBlocks[index].dead == 0) {
    HugeBlock *block = &hugeBlocks[index];
    block->dead = ++hugeReleases;
    /* A large block's ends are poisoned, its middle cleared rather than
     * written: the middle is mapped without access below, so a use there
     * faults, and its shadow costs no memory. */
    if (block->size > 2 * (size_t)TailLimit) {
      shadowDead(block->base, TailLimit);
      weavecRtShadowClear(block->base + TailLimit,
                          block->base + block->size - TailLimit);
      shadowDead(block->base + block->size - TailLimit, TailLimit);
    } else {
      shadowDead(block->base, block->size);
    }
    /* The memory goes back; the address range stays this block's until a
     * sweep finds no pointer to it. */
    (void)mmap((void *)block->map, block->mapBytes, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    ok = 1;
    ++hugeDeadCount;
    (void)__atomic_add_fetch(&hugeDeadBytes, block->size, __ATOMIC_RELAXED);
  }
  give(&hugeLock);
  return ok;
}

/*===-- The next allocator (section 2.5)
 * ------------------------------------===*/

#if defined(__APPLE__)

static void nextFree(void *p) {
  malloc_zone_t *zone = malloc_zone_from_ptr(p);
  if (zone == NULL)
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  malloc_zone_free(zone, p);
}

static void *nextRealloc(void *p, size_t size) {
  malloc_zone_t *zone = malloc_zone_from_ptr(p);
  if (zone == NULL)
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  return malloc_zone_realloc(zone, p, size);
}

static size_t nextSize(const void *p) {
  malloc_zone_t *zone = malloc_zone_from_ptr(p);
  return zone != NULL ? zone->size(zone, p) : 0;
}

static void *nextAlign(size_t alignment, size_t size) {
  return malloc_zone_memalign(malloc_default_zone(), alignment, size);
}

#else

extern void __libc_free(void *) __attribute__((weak));
extern void *__libc_realloc(void *, size_t) __attribute__((weak));
extern void *__libc_memalign(size_t, size_t) __attribute__((weak));
extern void *dlsym(void *, const char *) __attribute__((weak));
extern int dladdr(const void *, Dl_info *) __attribute__((weak));

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1L)
#endif

static void *nextSymbol(const char *name) {
  return dlsym != NULL ? dlsym(RTLD_NEXT, name) : NULL;
}

/* Whether `p`, which none of this runtime's blocks holds, is known not to
 * be the C library's either: it lies on the calling thread's stack or in a
 * loaded image (a global). The C library need not notice. */
static int notLibraryBlock(const void *p) {
  const uintptr_t address = (uintptr_t)p;
  const uintptr_t top = weavecRtStackTop();
  Dl_info info;
  if (top != 0 && address < top &&
      address >= (uintptr_t)__builtin_frame_address(0))
    return 1;
  return dladdr != NULL && dladdr(p, &info) != 0;
}

static void nextFree(void *p) {
  void (*release)(void *) = __libc_free;
  if (notLibraryBlock(p))
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  if (release == NULL)
    release = (void (*)(void *))nextSymbol("free");
  /* Without a next allocator the block is nobody's this runtime knows. */
  if (release == NULL)
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  release(p);
}

static void *nextRealloc(void *p, size_t size) {
  void *(*resize)(void *, size_t) = __libc_realloc;
  if (notLibraryBlock(p))
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  if (resize == NULL)
    resize = (void *(*)(void *, size_t))nextSymbol("realloc");
  if (resize == NULL)
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  return resize(p, size);
}

static size_t nextSize(const void *p) {
  size_t (*usable)(void *) =
      (size_t (*)(void *))nextSymbol("malloc_usable_size");
  /* The next definition after the runtime's is the C library's, which
   * answers; in a static program there is none. */
  extern size_t malloc_usable_size(void *);
  if (usable == NULL || usable == malloc_usable_size)
    return 0;
  return usable((void *)(uintptr_t)p);
}

static void *nextAlign(size_t alignment, size_t size) {
  return __libc_memalign != NULL ? __libc_memalign(alignment, size) : NULL;
}

#endif

/*===-- Allocate, release, reallocate (section 2.3)
 * -------------------------===*/

/* The cache of a call that is the thread's only one in the allocator; null
 * for a nested one (a signal handler's), which uses none. */
static ThreadCache *cacheFor(unsigned c) {
  ThreadCache *cache = c < cachedClasses ? threadCache() : NULL;
  return cache != NULL && cache->depth <= 1 ? cache : NULL;
}

static void *allocate(size_t size, size_t alignment) {
  unsigned region;
  void *block;
  if (!weavecRtInitialize()) {
    /* Without an arena (a limit on the address space), the C library's
     * blocks, zero-filled as the arena's are (RFC 0038, amendment 15). */
    block = nextAlign(alignment > 16 ? alignment : 16, size);
    if (block == NULL)
      errno = ENOMEM;
    return block != NULL ? memset(block, 0, size) : NULL;
  }
  if (alignment > 16) {
    /* The smallest power-of-two class above the size and at least the
     * alignment: its slots are aligned to their size. */
    size_t bytes = alignment;
    if ((alignment & (alignment - 1)) != 0) {
      errno = EINVAL;
      return NULL;
    }
    while (bytes <= size && bytes < ((size_t)1 << WeavecRtMaxClassShift))
      bytes <<= 1;
    region = bytes > size ? powerClass(bytes) : WeavecRtClassCount;
  } else {
    region = classFor(size);
  }
  weavecRtCount(WeavecRtStatAllocations);
  if (region < __weavec_rt_heap.classes) {
    ThreadCache *cache = cacheFor(region);
    block = cache != NULL ? cachedAlloc(cache, region, size)
                          : takeSlot(region, size);
    if (block != NULL)
      return block;
  }
  weavecRtCount(WeavecRtStatHuge);
  return hugeAlloc(size, alignment);
}

/* The requested size of the live block that starts at `p`, when the arena
 * or the huge table holds it. */
static int ownedSize(const void *p, size_t *size) {
  const uintptr_t address = (uintptr_t)p;
  if (inArena(address)) {
    const SlotRef ref = slotOf(address);
    const uint32_t word = wordOf(ref);
    if (ref.base != address || (word & WeavecRtWordState) != WeavecRtLive)
      return 0;
    *size = weavecRtWordValue(word);
    return 1;
  }
  {
    HugeBlock block;
    if (hugeFind(address, &block) && block.base == address && block.dead == 0) {
      *size = block.size;
      return 1;
    }
  }
  return 0;
}

static const char *whyNotReleasable(uintptr_t address) {
  const SlotRef ref = slotOf(address);
  const uint32_t word = wordOf(ref);
  if ((word & WeavecRtWordState) == WeavecRtDead)
    return "the block was already released";
  if ((word & WeavecRtWordState) != WeavecRtLive)
    return "no block is allocated there";
  return "not the start of its block";
}

static void release(void *p) {
  const uintptr_t address = (uintptr_t)p;
  if (inArena(address)) {
    const SlotRef ref = slotOf(address);
    const uint32_t word = wordOf(ref);
    const unsigned c = regionClass[ref.region];
    ThreadCache *cache;
    if (ref.base != address || (word & WeavecRtWordState) != WeavecRtLive)
      __weavec_rt_fatal("invalid release", p, whyNotReleasable(address));
    weavecRtCount(WeavecRtStatReleases);
    claim(ref, word, p);
    cache = cacheFor(c);
    if (cache != NULL)
      cachedRelease(cache, c, ref);
    else
      retire(ref);
    return;
  }
  if (hugeFree(address)) {
    if (overBudget())
      weavecRtSweep();
    return;
  }
  {
    HugeBlock block;
    if (hugeFind(address, &block))
      __weavec_rt_fatal("invalid release", p,
                        block.dead != 0 ? "the block was already released"
                                        : "not the start of its block");
  }
  nextFree(p);
}

/* RFC 0037 amendment 11: a call made while the thread is already inside
 * the allocator (by a signal handler, which C does not allow but programs
 * do) uses no cache and starts no sweep; while the thread holds an
 * allocator lock it allocates nothing and defers its release. */
void *__weavec_rt_alloc(size_t size, size_t alignment) {
  ThreadCache *self;
  void *block;
  self = ownCache();
  if (self != NULL && self->depth != 0 && self->locks != 0) {
    errno = ENOMEM;
    return NULL;
  }
  if (self != NULL)
    ++self->depth;
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  block = allocate(size, alignment);
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  if (self != NULL && --self->depth == 0)
    while (self->deferredCount != 0)
      __weavec_rt_free(self->deferred[--self->deferredCount]);
  return block;
}

void __weavec_rt_free(void *p) {
  ThreadCache *self;
  if (p == NULL)
    return;
  self = ownCache();
  if (self != NULL && self->depth != 0 && self->locks != 0) {
    /* (A release that does not fit waits for no one: the block stays.) */
    if (self->deferredCount < sizeof self->deferred / sizeof self->deferred[0])
      self->deferred[self->deferredCount++] = p;
    return;
  }
  if (self != NULL)
    ++self->depth;
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  release(p);
  __atomic_signal_fence(__ATOMIC_SEQ_CST);
  if (self != NULL && --self->depth == 0)
    while (self->deferredCount != 0)
      __weavec_rt_free(self->deferred[--self->deferredCount]);
}

int weavecRtAllocatorNested(void) {
  const ThreadCache *self = ownCache();
  return self != NULL && self->depth > 1;
}

void *__weavec_rt_realloc(void *p, size_t size) {
  const uintptr_t address = (uintptr_t)p;
  size_t old = 0;
  void *moved;
  if (p == NULL)
    return __weavec_rt_alloc(size, 0);
  if (inArena(address)) {
    const SlotRef ref = slotOf(address);
    uint32_t *word = &slotWords(ref.region)[ref.slot];
    const uint32_t value = wordOf(ref);
    if (ref.base != address || (value & WeavecRtWordState) != WeavecRtLive)
      __weavec_rt_fatal("invalid release", p, whyNotReleasable(address));
    old = weavecRtWordValue(value);
    if (classFor(size) == regionClass[ref.region]) {
      /* The block stays; bytes it gains are zero (section 2.3). */
      if (size > old)
        memset((char *)p + old, 0, size - old);
      shadowLive(address, size, __weavec_rt_heap.table[ref.region].size);
      storeWord(word, ((uint32_t)size << WeavecRtWordShift) | WeavecRtLive);
      return p;
    }
  } else if (!ownedSize(p, &old)) {
    HugeBlock block;
    if (hugeFind(address, &block))
      __weavec_rt_fatal("invalid release", p, "not the start of a live block");
    return nextRealloc(p, size);
  }
  moved = __weavec_rt_alloc(size, 0);
  if (moved == NULL)
    return NULL;
  memcpy(moved, p, old < size ? old : size);
  __weavec_rt_free(p);
  return moved;
}

size_t __weavec_rt_size(const void *p) {
  size_t size = 0;
  if (p == NULL)
    return 0;
  if (ownedSize(p, &size))
    return size;
  /* (The huge blocks' address span holds other allocators' blocks too: only
   * a pointer into one of them is the runtime's.) */
  {
    HugeBlock block;
    if (inArena((uintptr_t)p) || hugeFind((uintptr_t)p, &block))
      return 0;
  }
  return nextSize(p);
}

/*===-- Reports
 * ------------------------------------------------------------===*/

struct weavecRtHeapObject weavecRtHeapObjectAt(uintptr_t address) {
  struct weavecRtHeapObject object = {0, 0, 0, 0};
  if (inArena(address)) {
    const SlotRef ref = slotOf(address);
    const uint32_t word = wordOf(ref);
    object.found = (word & WeavecRtWordState) != WeavecRtNever;
    object.live = (word & WeavecRtWordState) == WeavecRtLive;
    object.base = ref.base;
    /* A released slot: its whole extent. */
    object.size = object.live ? weavecRtWordValue(word)
                              : __weavec_rt_heap.table[ref.region].size;
    return object;
  }
  {
    HugeBlock block;
    if (hugeFind(address, &block)) {
      object.found = 1;
      object.live = block.dead == 0;
      object.base = block.base;
      object.size = block.size;
    }
  }
  return object;
}

/*===-- fork
 * ----------------------------------------------------------------===*/

void weavecRtLockAll(void) {
  unsigned i;
  weavecRtLock(&initLock);
  weavecRtLock(&spareLock);
  for (i = 0; i < WeavecRtRegionCount; ++i)
    weavecRtLock(&regionState[i].lock);
  weavecRtLock(&hugeLock);
}

void weavecRtUnlockAll(void) {
  unsigned i;
  weavecRtUnlock(&hugeLock);
  for (i = WeavecRtRegionCount; i-- > 0;)
    weavecRtUnlock(&regionState[i].lock);
  weavecRtUnlock(&spareLock);
  weavecRtUnlock(&initLock);
}

/* The sweep's lock comes first, so that a fork never copies a sweep in
 * progress. */
static void forkPrepare(void) {
  weavecRtSweepLock();
  weavecRtLock(&cacheListLock);
  weavecRtLockAll();
}
static void forkParent(void) {
  weavecRtUnlockAll();
  weavecRtUnlock(&cacheListLock);
  weavecRtSweepUnlock();
}
/* The child has one thread: the other threads' caches go back to the
 * shared lists (RFC 0037 section 2.1). */
static void forkChild(void) {
  ThreadCache *own = NULL;
  ThreadCache *cache;
  weavecRtUnlockAll();
  if (__atomic_load_n(&cacheKeyReady, __ATOMIC_ACQUIRE))
    own = (ThreadCache *)pthread_getspecific(cacheKey);
  cache = caches;
  caches = NULL;
  while (cache != NULL) {
    ThreadCache *next = cache->next;
    if (cache == own) {
      cache->next = caches;
      caches = cache;
    } else {
      drainCache(cache, 1);
      (void)munmap(cache, sizeof(ThreadCache));
    }
    cache = next;
  }
  weavecRtUnlock(&cacheListLock);
  weavecRtSweepForkChild();
  weavecRtSweepUnlock();
}

/*===-- The allocator's side of the sweep (RFC 0036 section 3.4)
 * -----------===*/

/* Whether the quarantine or the released huge blocks hold anything. */
int weavecRtSweepWanted(void) {
  return __atomic_load_n(&quarantineBytes, __ATOMIC_RELAXED) != 0 ||
         __atomic_load_n(&hugeDeadCount, __ATOMIC_RELAXED) != 0;
}

/* Maps the marks of each class whose quarantine holds slots. Every lock is
 * held. */
int weavecRtPrepareMarks(void) {
  unsigned region;
  for (region = 0; region < __weavec_rt_heap.regions; ++region) {
    RegionState *state = &regionState[region];
    if (state->deadHead == 0 || state->marks != NULL)
      continue;
    {
      const size_t bytes =
          ((size_t)__weavec_rt_heap.table[region].capacity + 63) / 64 * 8;
      void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
      if (map == MAP_FAILED)
        return 0;
      weavecRtInternal((uintptr_t)map, (uintptr_t)map + bytes);
      state->marks = (uint64_t *)map;
    }
  }
  return 1;
}

/* Marks the quarantined slot or released huge block `value` points into. */
static inline void markAddress(uintptr_t value) {
  if (value - __weavec_rt_heap.base < __weavec_rt_heap.bytes) {
    /* A class with nothing in its quarantine needs no slot index. */
    const RegionState *quick =
        &regionState[(value - __weavec_rt_heap.base) >> __weavec_rt_heap.shift];
    SlotRef ref;
    if (quick->marks == NULL || quick->deadHead == 0)
      return;
    ref = slotOf(value);
    /* Atomic: mark workers set bits of one word at once (RFC 0037
     * section 3). */
    if ((wordOf(ref) & WeavecRtWordState) == WeavecRtDead)
      (void)__atomic_fetch_or(&regionState[ref.region].marks[ref.slot >> 6],
                              (uint64_t)1 << (ref.slot & 63), __ATOMIC_RELAXED);
    return;
  }
  if (hugeDeadCount != 0 && value >= hugeLow && value < hugeHigh) {
    const long index = hugeIndex(value);
    if (index >= 0 && hugeBlocks[index].dead != 0 &&
        value < hugeBlocks[index].map + hugeBlocks[index].mapBytes)
      hugeBlocks[index].marked = 1;
  }
}

void weavecRtMarkWords(uintptr_t low, uintptr_t high) {
  const uintptr_t *word = (const uintptr_t *)((low + 7) & ~(uintptr_t)7);
  const uintptr_t *end = (const uintptr_t *)(high & ~(uintptr_t)7);
  for (; word < end; ++word)
    markAddress(*word);
}

void weavecRtMarkSlots(unsigned region, uint32_t first, uint32_t last) {
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[region];
  const uint32_t *words = slotWords(region);
  uint32_t slot;
  for (slot = first; slot < last; ++slot) {
    const uint32_t word = loadWord(&words[slot]);
    if ((word & WeavecRtWordState) == WeavecRtLive) {
      const uintptr_t base =
          regionStart(region) + (uintptr_t)slot * entry->size;
      weavecRtMarkWords(base, base + weavecRtWordValue(word));
    }
  }
}

void weavecRtHeapWork(void (*add)(unsigned, uint32_t, uint32_t), size_t chunk) {
  unsigned region;
  for (region = 0; region < __weavec_rt_heap.regions; ++region) {
    const size_t size = __weavec_rt_heap.table[region].size;
    const uint32_t bump = regionState[region].bump;
    const uint32_t step = size >= chunk ? 1 : (uint32_t)(chunk / size);
    uint32_t first;
    for (first = 0; first < bump; first += step)
      add(region, first, bump - first > step ? first + step : bump);
  }
}

void weavecRtMarkHuge(void) {
  size_t i;
  for (i = 0; i < hugeCount; ++i)
    if (hugeBlocks[i].dead == 0)
      weavecRtMarkWords(hugeBlocks[i].base,
                        hugeBlocks[i].base + hugeBlocks[i].size);
}

void weavecRtMarkHeap(void) {
  unsigned region;
  for (region = 0; region < __weavec_rt_heap.regions; ++region)
    weavecRtMarkSlots(region, 0, regionState[region].bump);
  weavecRtMarkHuge();
}

size_t weavecRtLiveBytes(void) {
  return liveNow();
}

void weavecRtRecycle(struct weavecRtSweepCounts *counts, int recycle) {
  unsigned region;
  size_t i;
  for (region = 0; region < __weavec_rt_heap.classes; ++region)
    classGained[region] = 0;
  for (region = 0; region < __weavec_rt_heap.regions; ++region) {
    RegionState *state = &regionState[region];
    uint32_t *words = slotWords(region);
    const size_t size = __weavec_rt_heap.table[region].size;
    uint32_t keptHead = 0;
    uint32_t slot;
    if (state->marks == NULL || state->deadHead == 0)
      continue;
    /* By slot, from the last: the free list and the quarantine come out in
     * address order, so that slots are reused, and their words read, in
     * order. */
    for (slot = state->bump; slot-- > 0;) {
      const uint32_t word = loadWord(&words[slot]);
      /* A slot in a thread's cache, or being released, is not queued: it
       * stays as it is (RFC 0037 section 2.1). */
      if ((word & WeavecRtWordState) != WeavecRtDead ||
          weavecRtWordValue(word) == CachedDead)
        continue;
      if (!recycle || (state->marks[slot >> 6] >> (slot & 63) & 1) != 0) {
        if (keptHead == 0)
          state->deadTail = slot + 1;
        storeWord(&words[slot], (keptHead << WeavecRtWordShift) | WeavecRtDead);
        keptHead = slot + 1;
        ++counts->retained;
        counts->retainedBytes += size;
      } else {
        storeWord(&words[slot],
                  (state->freeHead << WeavecRtWordShift) | WeavecRtFree);
        state->freeHead = slot + 1;
        quarantineBytes -= size;
        ++counts->recycled;
        weavecRtCount(WeavecRtStatRecycled);
      }
    }
    state->deadHead = keptHead;
    if (keptHead == 0)
      state->deadTail = 0;
    memset(state->marks, 0, ((size_t)state->bump + 63) / 64 * 8);
  }
  for (i = 0; i < hugeCount;) {
    HugeBlock *block = &hugeBlocks[i];
    if (block->dead == 0 || block->marked || !recycle) {
      if (block->dead != 0) {
        ++counts->retained;
        counts->retainedBytes += block->size;
      }
      block->marked = 0;
      ++i;
      continue;
    }
    weavecRtShadowClear(block->base, block->base + block->size + 16);
    (void)munmap((void *)block->map, block->mapBytes);
    hugeDeadBytes -= block->size;
    memmove(block, block + 1, (hugeCount - i - 1) * sizeof(HugeBlock));
    --hugeCount;
    --hugeDeadCount;
    ++counts->recycled;
  }
  __atomic_store_n(&retainedBytes, quarantineBytes + hugeDeadBytes,
                   __ATOMIC_RELAXED);
}

/*===-- The Darwin zone (section 2.5)
 * ---------------------------------------===*/

#if defined(__APPLE__)

#include <mach/mach.h>

static size_t zoneSize(malloc_zone_t *zone, const void *p) {
  size_t size = 0;
  (void)zone;
  /* 0 means "not this zone's", so a zero-size block answers 1. */
  if (!ownedSize(p, &size))
    return 0;
  return size != 0 ? size : 1;
}

static void *zoneMalloc(malloc_zone_t *zone, size_t size) {
  (void)zone;
  return __weavec_rt_alloc(size, 0);
}

static void *zoneCalloc(malloc_zone_t *zone, size_t count, size_t size) {
  size_t bytes;
  (void)zone;
  if (__builtin_mul_overflow(count, size, &bytes)) {
    errno = ENOMEM;
    return NULL;
  }
  return __weavec_rt_alloc(bytes, 0);
}

static void *zoneValloc(malloc_zone_t *zone, size_t size) {
  (void)zone;
  return __weavec_rt_alloc(size, pageBytes);
}

static void *zoneMemalign(malloc_zone_t *zone, size_t alignment, size_t size) {
  (void)zone;
  return __weavec_rt_alloc(size, alignment);
}

static void zoneFree(malloc_zone_t *zone, void *p) {
  (void)zone;
  __weavec_rt_free(p);
}

static void zoneFreeDefinite(malloc_zone_t *zone, void *p, size_t size) {
  (void)zone;
  (void)size;
  __weavec_rt_free(p);
}

static void *zoneRealloc(malloc_zone_t *zone, void *p, size_t size) {
  (void)zone;
  return __weavec_rt_realloc(p, size);
}

static void zoneDestroy(malloc_zone_t *zone) {
  (void)zone;
}

static unsigned zoneBatchMalloc(malloc_zone_t *zone, size_t size,
                                void **results, unsigned count) {
  (void)zone;
  (void)size;
  (void)results;
  (void)count;
  return 0;
}

static void zoneBatchFree(malloc_zone_t *zone, void **blocks, unsigned count) {
  unsigned i;
  (void)zone;
  for (i = 0; i < count; ++i)
    __weavec_rt_free(blocks[i]);
}

static size_t zonePressureRelief(malloc_zone_t *zone, size_t goal) {
  (void)zone;
  (void)goal;
  return 0;
}

static boolean_t zoneClaimedAddress(malloc_zone_t *zone, void *p) {
  HugeBlock block;
  (void)zone;
  return inArena((uintptr_t)p) || hugeFind((uintptr_t)p, &block);
}

static kern_return_t zoneEnumerator(task_t task, void *context,
                                    unsigned typeMask, vm_address_t address,
                                    memory_reader_t reader,
                                    vm_range_recorder_t recorder) {
  (void)task;
  (void)context;
  (void)typeMask;
  (void)address;
  (void)reader;
  (void)recorder;
  return KERN_SUCCESS;
}

static size_t zoneGoodSize(malloc_zone_t *zone, size_t size) {
  (void)zone;
  return size;
}

static boolean_t zoneCheck(malloc_zone_t *zone) {
  (void)zone;
  return 1;
}

static void zonePrint(malloc_zone_t *zone, boolean_t verbose) {
  (void)zone;
  (void)verbose;
}

static void zoneLog(malloc_zone_t *zone, void *address) {
  (void)zone;
  (void)address;
}

static void zoneForceLock(malloc_zone_t *zone) {
  (void)zone;
  forkPrepare();
}

static void zoneForceUnlock(malloc_zone_t *zone) {
  (void)zone;
  forkParent();
}

static void zoneStatistics(malloc_zone_t *zone, malloc_statistics_t *stats) {
  (void)zone;
  memset(stats, 0, sizeof *stats);
}

static boolean_t zoneLocked(malloc_zone_t *zone) {
  (void)zone;
  return 0;
}

static void zoneReinitLock(malloc_zone_t *zone) {
  (void)zone;
  forkChild();
}

static malloc_introspection_t zoneIntrospection;
static malloc_zone_t arenaZone;

static void registerZone(void) {
  zoneIntrospection.enumerator = zoneEnumerator;
  zoneIntrospection.good_size = zoneGoodSize;
  zoneIntrospection.check = zoneCheck;
  zoneIntrospection.print = zonePrint;
  zoneIntrospection.log = zoneLog;
  zoneIntrospection.force_lock = zoneForceLock;
  zoneIntrospection.force_unlock = zoneForceUnlock;
  zoneIntrospection.statistics = zoneStatistics;
  zoneIntrospection.zone_locked = zoneLocked;
  zoneIntrospection.reinit_lock = zoneReinitLock;

  arenaZone.size = zoneSize;
  arenaZone.malloc = zoneMalloc;
  arenaZone.calloc = zoneCalloc;
  arenaZone.valloc = zoneValloc;
  arenaZone.free = zoneFree;
  arenaZone.realloc = zoneRealloc;
  arenaZone.destroy = zoneDestroy;
  arenaZone.zone_name = "weavec";
  arenaZone.batch_malloc = zoneBatchMalloc;
  arenaZone.batch_free = zoneBatchFree;
  arenaZone.introspect = &zoneIntrospection;
  /* Version 10: the fields through `claimed_address`. */
  arenaZone.version = 10;
  arenaZone.memalign = zoneMemalign;
  arenaZone.free_definite_size = zoneFreeDefinite;
  arenaZone.pressure_relief = zonePressureRelief;
  arenaZone.claimed_address = zoneClaimedAddress;
  malloc_zone_register(&arenaZone);
}

/* The zone malloc() serves from: the first registered one. */
static malloc_zone_t *firstZone(void) {
  vm_address_t *zones = NULL;
  unsigned count = 0;
  if (malloc_get_all_zones(0, NULL, &zones, &count) != KERN_SUCCESS ||
      count == 0)
    return malloc_default_zone();
  return (malloc_zone_t *)zones[0];
}

/* RFC 0033 section 6.2: the arena's zone becomes the process's default, so
 * that malloc() from every image, the C library's own calls included
 * (strdup, getline, asprintf), is served by the arena. Registering a zone
 * appends it: the zones before the arena's are moved to the end, one at a
 * time, until it is first (jemalloc's zone_promote moves the system's
 * zones the same way). Blocks the system's zones allocated before stay
 * theirs; the next allocator releases them. */
static void promoteZone(void) {
  unsigned tries;
  for (tries = 0; tries < 64; ++tries) {
    malloc_zone_t *first = firstZone();
    if (first == &arenaZone)
      return;
    malloc_zone_unregister(first);
    malloc_zone_register(first);
  }
}

#endif
