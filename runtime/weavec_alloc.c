/*===- weavec_alloc.c - The size-class arena allocator -------------*- C -*-===*\
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
|* RFC 0034, section 2: a shadow byte per 16-byte granule of the regions says
|* which bytes are a live object's, so that a guard asks one load (see
|* weavec_rt.h for the encoding). The allocator writes it whenever a slot
|* word changes.
|*
|* A request takes a slot of the smallest class strictly larger than it, so
|* at least one byte after every object belongs to no object. Slot words,
|* the free lists and the quarantine queues live in the metadata, never in
|* freed memory. Released slots wait in a quarantine before they are reused;
|* requests no class can hold are mapped on their own and kept in a table.
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
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/* The bytes the guards read for every address until the shadow exists (a
 * guard of a crossing access reads the second). */
static const unsigned char deadBytes[2] = {WeavecRtShadowDead,
                                           WeavecRtShadowDead};

/* Until the shadow exists `mask` is 0, so the guards' index into it is
 * always 0: a dead byte, and the runtime answers. Until the arena exists
 * `bytes` is 0, so every pointer is outside it. */
struct __weavec_rt_heap_t __weavec_rt_heap = {
    0, 0, (uintptr_t)deadBytes, 0, 0, 0, 0, {{0, 0, 0, 0}}};

/*===-- State ---------------------------------------------------------------===*/

/* What one class needs besides the public table; guarded by `lock`. */
typedef struct {
  WeavecRtLock lock;
  /* Slots handed out so far from the region's start. */
  uint32_t bump;
  /* Slot index + 1 of the first free slot; 0 for none. */
  uint32_t freeHead;
  /* The quarantine queue: slot index + 1 of its oldest and newest slot. */
  uint32_t deadHead;
  uint32_t deadTail;
  /* Bytes of the region, and of its metadata, that are accessible. */
  uintptr_t committed;
  uintptr_t metaCommitted;
} ClassState;

static ClassState classState[WeavecRtClassCount];

/* A block mapped on its own (section 2.3). */
typedef struct {
  uintptr_t base;
  size_t size;
  uintptr_t map;
  size_t mapBytes;
  /* 0 while live; else the order of its release. */
  unsigned long long dead;
} HugeBlock;

static WeavecRtLock initLock;
static WeavecRtLock hugeLock;
static HugeBlock *hugeBlocks;
static size_t hugeCount;
static size_t hugeCapacity;
/* The lowest and highest address a huge block has had: a quick rejection. */
static uintptr_t hugeLow = ~(uintptr_t)0;
static uintptr_t hugeHigh;
/* Released huge blocks stay mapped, without access, until this many newer
 * ones were released: a stale pointer then finds a dead block, and nothing
 * else can be mapped there. */
enum { HugeTombstones = 32 };
static size_t hugeDeadCount;
static unsigned long long hugeReleases;

static size_t pageBytes;
/* RFC 0034, section 5.3. */
static size_t quarantineBudget = (size_t)16 << 20;
static size_t quarantineBytes;
static unsigned sweepCursor;

enum { DataChunk = 1 << 20, MetaChunk = 64 * 1024 };

/*===-- Classes -------------------------------------------------------------===*/

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
    const unsigned k = (unsigned)(63 - __builtin_clzll((unsigned long long)(need - 1))) - 2;
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
    /* Normalise so that m is odd or small: the divisor of the slot index. */
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

/*===-- The shadow (RFC 0034, section 2) -----------------------------------===*/

/* Reserves the shadow of the address space: 2^48 bytes of it where the
 * system allows, else 2^47, 2^44 (39-bit address spaces are 2^35). Pages no
 * allocation wrote read 0, which no live arena object's bytes are. */
static void reserveShadow(void) {
  static const unsigned Bits[] = {48, 47, 44, 39};
  unsigned i;
  for (i = 0; i < sizeof Bits / sizeof Bits[0]; ++i) {
    const uintptr_t bytes = (uintptr_t)1 << (Bits[i] - 4);
    void *map = mmap(NULL, bytes + 64, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (map == MAP_FAILED)
      continue;
    __atomic_store_n(&__weavec_rt_heap.shadow, (uintptr_t)map,
                     __ATOMIC_RELEASE);
    /* Published last: until then every guard asks the runtime. */
    __atomic_store_n(&__weavec_rt_heap.mask, bytes - 1, __ATOMIC_RELEASE);
    return;
  }
}

static inline unsigned char *shadowOf(uintptr_t address) {
  return weavecRtShadowOf(address);
}

static void shadowLive(uintptr_t base, size_t size) {
  weavecRtShadowLive(base, size);
}

static void shadowDead(uintptr_t base, size_t size) {
  weavecRtShadowFill(base, size, WeavecRtShadowDead);
}

static inline uint32_t loadWord(const uint32_t *word) {
  return __atomic_load_n(word, __ATOMIC_RELAXED);
}

static inline void storeWord(uint32_t *word, uint32_t value) {
  __atomic_store_n(word, value, __ATOMIC_RELAXED);
}

/*===-- Initialisation ------------------------------------------------------===*/

static void forkPrepare(void);
static void forkParent(void);
static void forkChild(void);
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
  buildClasses(shift);
  data = (uintptr_t)__weavec_rt_heap.classes << shift;
  meta = (uintptr_t)__weavec_rt_heap.classes << (shift - 2);
  align = (uintptr_t)1 << shift;
  total = data + meta + align;
  map = mmap(NULL, total, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED)
    return 0;
  /* Regions start at multiples of their size, so that the slots of a
   * power-of-two class are aligned to it. */
  start = ((uintptr_t)map + align - 1) & ~(align - 1);
  /* A guard reads a slot word without asking how far its region has got:
   * the words are readable from the start, and zero (never allocated)
   * until written. */
  if (mprotect((void *)(start + data), meta, PROT_READ) != 0) {
    (void)munmap(map, total);
    return 0;
  }
  __weavec_rt_heap.shift = shift;
  __weavec_rt_heap.meta = start + data;
  __atomic_store_n(&__weavec_rt_heap.base, start, __ATOMIC_RELEASE);
  /* Published last: until then every pointer is outside the arena. */
  __atomic_store_n(&__weavec_rt_heap.bytes, data, __ATOMIC_RELEASE);
  return 1;
}

int weavecRtInitialise(void) {
  int ok = 1;
  if (__atomic_load_n(&__weavec_rt_heap.bytes, __ATOMIC_ACQUIRE) != 0)
    return 1;
  weavecRtLock(&initLock);
  if (__weavec_rt_heap.bytes == 0) {
    const long page = sysconf(_SC_PAGESIZE);
    pageBytes = page > 0 ? (size_t)page : 4096;
    reserveShadow();
    ok = reserve(32) || reserve(30);
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

/*===-- Slots ---------------------------------------------------------------===*/

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
  *committed = target;
  return 1;
}

/* A slot of `region` for an object of `size` bytes, live; null when the
 * region is exhausted or memory is. `recycled` says the slot was used
 * before, so its bytes are not known to be zero. */
static void *takeSlot(unsigned region, size_t size, int *recycled) {
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[region];
  ClassState *state = &classState[region];
  uint32_t *words = slotWords(region);
  uint32_t slot;
  *recycled = 0;
  weavecRtLock(&state->lock);
  if (state->freeHead != 0) {
    slot = state->freeHead - 1;
    state->freeHead = loadWord(&words[slot]) >> 2;
    *recycled = 1;
  } else {
    const uintptr_t limit = (uintptr_t)1 << __weavec_rt_heap.shift;
    slot = state->bump;
    if (slot >= entry->capacity ||
        !commit(regionStart(region), &state->committed,
                ((uintptr_t)slot + 1) * entry->size, DataChunk, limit) ||
        !commit((uintptr_t)words, &state->metaCommitted,
                ((uintptr_t)slot + 1) * sizeof(uint32_t), MetaChunk,
                limit >> 2)) {
      weavecRtUnlock(&state->lock);
      return NULL;
    }
    __atomic_store_n(&state->bump, slot + 1, __ATOMIC_RELAXED);
  }
  storeWord(&words[slot], ((uint32_t)size << 2) | WeavecRtLive);
  weavecRtUnlock(&state->lock);
  return (void *)(regionStart(region) + (uintptr_t)slot * entry->size);
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
  ref.slot =
      entry->magic == 0 ? (uint32_t)units : (uint32_t)((units * entry->magic) >> 32);
  ref.base = regionStart(region) + (uintptr_t)ref.slot * entry->size;
  return ref;
}

/* The word of a slot; "never allocated" for one the region has not
 * reached. */
static inline uint32_t wordOf(SlotRef ref) {
  return loadWord(&slotWords(ref.region)[ref.slot]);
}

/*===-- The quarantine (section 2.4) ----------------------------------------===*/

/* Read in a constructor, never by `free`: the C library frees with its
 * environment lock held (`unsetenv`), and `getenv` takes it again. */
__attribute__((constructor)) static void readQuarantineBudget(void) {
  const char *text = getenv("WEAVEC_RT_QUARANTINE");
  if (text != NULL && *text >= '0' && *text <= '9')
    quarantineBudget = (size_t)strtoull(text, NULL, 10);
}

/* Moves the oldest dead slot of a class to its free list. The class's lock
 * is held. False when the class has none. */
static int recycleOldest(unsigned region) {
  ClassState *state = &classState[region];
  uint32_t *words = slotWords(region);
  uint32_t slot;
  if (state->deadHead == 0)
    return 0;
  slot = state->deadHead - 1;
  state->deadHead = loadWord(&words[slot]) >> 2;
  if (state->deadHead == 0)
    state->deadTail = 0;
  storeWord(&words[slot], (state->freeHead << 2) | WeavecRtFree);
  state->freeHead = slot + 1;
  weavecRtCount(WeavecRtStatRecycled);
  (void)__atomic_sub_fetch(&quarantineBytes, __weavec_rt_heap.table[region].size,
                           __ATOMIC_RELAXED);
  return 1;
}

static inline int overBudget(void) {
  return __atomic_load_n(&quarantineBytes, __ATOMIC_RELAXED) > quarantineBudget;
}

/* Gives the whole pages inside a dead slot back to the system. */
static void giveBack(uintptr_t base, size_t bytes) {
  const uintptr_t first = (base + pageBytes - 1) & ~(uintptr_t)(pageBytes - 1);
  const uintptr_t last = (base + bytes) & ~(uintptr_t)(pageBytes - 1);
  if (last <= first)
    return;
#if defined(__APPLE__)
  (void)madvise((void *)first, last - first, MADV_FREE);
#else
  (void)madvise((void *)first, last - first, MADV_DONTNEED);
#endif
}

/* The pages of a dead slot, and the whole pages of its shadow, which reads
 * 0 already (`retire` cleared it). */
static void decommit(uintptr_t base, size_t bytes) {
  giveBack(base, bytes);
  if (__weavec_rt_heap.mask != 0)
    giveBack((uintptr_t)shadowOf(base), bytes >> 4);
}

/* Makes the live slot `ref` dead and queues it. */
static void retire(SlotRef ref) {
  const struct __weavec_rt_class *entry = &__weavec_rt_heap.table[ref.region];
  ClassState *state = &classState[ref.region];
  uint32_t *words = slotWords(ref.region);
  /* Before the slot is queued: once it is, another thread may recycle it. */
  shadowDead(ref.base, loadWord(&words[ref.slot]) >> 2);
  if (entry->size >= WeavecRtDecommitBytes)
    decommit(ref.base, entry->size);
  weavecRtLock(&state->lock);
  storeWord(&words[ref.slot], WeavecRtDead);
  if (state->deadTail != 0)
    storeWord(&words[state->deadTail - 1],
              ((ref.slot + 1) << 2) | WeavecRtDead);
  else
    state->deadHead = ref.slot + 1;
  state->deadTail = ref.slot + 1;
  (void)__atomic_add_fetch(&quarantineBytes, entry->size, __ATOMIC_RELAXED);
  while (overBudget() && recycleOldest(ref.region)) {
  }
  weavecRtUnlock(&state->lock);
  /* Other classes hold the rest of an exceeded budget. */
  {
    unsigned tries;
    for (tries = 0; overBudget() && tries < __weavec_rt_heap.classes; ++tries) {
      const unsigned victim =
          __atomic_fetch_add(&sweepCursor, 1, __ATOMIC_RELAXED) %
          __weavec_rt_heap.classes;
      weavecRtLock(&classState[victim].lock);
      while (overBudget() && recycleOldest(victim)) {
      }
      weavecRtUnlock(&classState[victim].lock);
    }
  }
}

/*===-- Huge blocks ---------------------------------------------------------===*/

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
  if (size > (~(size_t)0 >> 1) - 2 * alignment - 2 * page) {
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

  weavecRtLock(&hugeLock);
  if (hugeCount == hugeCapacity) {
    const size_t capacity = hugeCapacity != 0 ? hugeCapacity * 2 : 256;
    HugeBlock *grown =
        (HugeBlock *)mmap(NULL, capacity * sizeof(HugeBlock),
                          PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS,
                          -1, 0);
    if ((void *)grown == MAP_FAILED) {
      weavecRtUnlock(&hugeLock);
      (void)munmap((void *)map, mapBytes);
      errno = ENOMEM;
      return NULL;
    }
    if (hugeCount != 0)
      memcpy(grown, hugeBlocks, hugeCount * sizeof(HugeBlock));
    if (hugeBlocks != NULL)
      (void)munmap(hugeBlocks, hugeCapacity * sizeof(HugeBlock));
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
  ++hugeCount;
  shadowLive(base, size);
  if (map < __atomic_load_n(&hugeLow, __ATOMIC_RELAXED))
    __atomic_store_n(&hugeLow, map, __ATOMIC_RELAXED);
  if (map + mapBytes > __atomic_load_n(&hugeHigh, __ATOMIC_RELAXED))
    __atomic_store_n(&hugeHigh, map + mapBytes, __ATOMIC_RELAXED);
  weavecRtUnlock(&hugeLock);
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
  weavecRtLock(&hugeLock);
  index = hugeIndex(address);
  if (index >= 0 && address < hugeBlocks[index].map + hugeBlocks[index].mapBytes) {
    *out = hugeBlocks[index];
    found = 1;
  }
  weavecRtUnlock(&hugeLock);
  return found;
}

/* Releases the live huge block that starts at `address`; false when there
 * is none. */
static int hugeFree(uintptr_t address) {
  HugeBlock gone;
  long index;
  int ok = 0;
  int unmap = 0;
  if (!maybeHuge(address))
    return 0;
  weavecRtLock(&hugeLock);
  index = hugeIndex(address);
  if (index >= 0 && hugeBlocks[index].base == address &&
      hugeBlocks[index].dead == 0) {
    HugeBlock *block = &hugeBlocks[index];
    block->dead = ++hugeReleases;
    weavecRtShadowFill(block->base, block->size, WeavecRtShadowHugeDead);
    /* The memory goes back; the address range stays this block's. */
    (void)mmap((void *)block->map, block->mapBytes, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1, 0);
    ok = 1;
    if (++hugeDeadCount > HugeTombstones) {
      size_t i;
      size_t oldest = hugeCount;
      for (i = 0; i < hugeCount; ++i)
        if (hugeBlocks[i].dead != 0 &&
            (oldest == hugeCount ||
             hugeBlocks[i].dead < hugeBlocks[oldest].dead))
          oldest = i;
      gone = hugeBlocks[oldest];
      memmove(&hugeBlocks[oldest], &hugeBlocks[oldest + 1],
              (hugeCount - oldest - 1) * sizeof(HugeBlock));
      --hugeCount;
      --hugeDeadCount;
      unmap = 1;
    }
  }
  weavecRtUnlock(&hugeLock);
  if (unmap) {
    weavecRtShadowFill(gone.base, gone.size, WeavecRtShadowDead);
    (void)munmap((void *)gone.map, gone.mapBytes);
  }
  return ok;
}

/*===-- The next allocator (section 2.5) ------------------------------------===*/

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

#else

extern void __libc_free(void *) __attribute__((weak));
extern void *__libc_realloc(void *, size_t) __attribute__((weak));
extern void *dlsym(void *, const char *) __attribute__((weak));

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1L)
#endif

static void *nextSymbol(const char *name) {
  return dlsym != NULL ? dlsym(RTLD_NEXT, name) : NULL;
}

static void nextFree(void *p) {
  void (*release)(void *) = __libc_free;
  if (release == NULL)
    release = (void (*)(void *))nextSymbol("free");
  /* Without a next allocator the block is nobody's this runtime knows. */
  if (release == NULL)
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  release(p);
}

static void *nextRealloc(void *p, size_t size) {
  void *(*resize)(void *, size_t) = __libc_realloc;
  if (resize == NULL)
    resize = (void *(*)(void *, size_t))nextSymbol("realloc");
  if (resize == NULL)
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  return resize(p, size);
}

static size_t nextSize(const void *p) {
  size_t (*usable)(void *) = (size_t (*)(void *))nextSymbol("malloc_usable_size");
  /* The runtime's own definition is the first in lookup order when it
   * interposes, and it would ask again. */
  extern size_t malloc_usable_size(void *) __attribute__((weak));
  if (usable == NULL || usable == malloc_usable_size)
    return 0;
  return usable((void *)(uintptr_t)p);
}

#endif

/*===-- Allocate, release, reallocate (section 2.3) -------------------------===*/

void *__weavec_rt_alloc(size_t size, size_t alignment) {
  unsigned region;
  void *block;
  int recycled = 0;
  WEAVEC_RT_FORWARD(alloc, size, alignment);
  if (!weavecRtInitialise()) {
    errno = ENOMEM;
    return NULL;
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
    block = takeSlot(region, size, &recycled);
    if (block != NULL) {
      if (recycled && size != 0)
        memset(block, 0, size);
      shadowLive((uintptr_t)block, size);
      return block;
    }
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
    if (ref.base != address || (word & 3) != WeavecRtLive)
      return 0;
    *size = word >> 2;
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
  if ((word & 3) == WeavecRtDead)
    return "the block was already released";
  if ((word & 3) != WeavecRtLive)
    return "no block is allocated there";
  return "not the start of its block";
}

void __weavec_rt_free(void *p) {
  const uintptr_t address = (uintptr_t)p;
  WEAVEC_RT_FORWARD_VOID(release, p);
  if (p == NULL)
    return;
  if (inArena(address)) {
    const SlotRef ref = slotOf(address);
    const uint32_t word = wordOf(ref);
    if (ref.base != address || (word & 3) != WeavecRtLive)
      __weavec_rt_fatal("invalid release", p, whyNotReleasable(address));
    weavecRtCount(WeavecRtStatReleases);
    retire(ref);
    return;
  }
  if (hugeFree(address))
    return;
  {
    HugeBlock block;
    if (hugeFind(address, &block))
      __weavec_rt_fatal("invalid release", p,
                        block.dead != 0 ? "the block was already released"
                                   : "not the start of its block");
  }
  if (weavecRtIsStackOrGlobal(p))
    __weavec_rt_fatal("invalid release", p, "not a heap block");
  nextFree(p);
}

void *__weavec_rt_realloc(void *p, size_t size) {
  const uintptr_t address = (uintptr_t)p;
  size_t old = 0;
  void *moved;
  WEAVEC_RT_FORWARD(realloc, p, size);
  if (p == NULL)
    return __weavec_rt_alloc(size, 0);
  if (inArena(address)) {
    const SlotRef ref = slotOf(address);
    uint32_t *word = &slotWords(ref.region)[ref.slot];
    const uint32_t value = wordOf(ref);
    if (ref.base != address || (value & 3) != WeavecRtLive)
      __weavec_rt_fatal("invalid release", p, whyNotReleasable(address));
    old = value >> 2;
    if (classFor(size) == ref.region) {
      /* The block stays; bytes it gains are zero (section 2.3). */
      if (size > old)
        memset((char *)p + old, 0, size - old);
      if (size < old)
        shadowDead(address, old);
      shadowLive(address, size);
      storeWord(word, ((uint32_t)size << 2) | WeavecRtLive);
      return p;
    }
  } else if (!ownedSize(p, &old)) {
    HugeBlock block;
    if (hugeFind(address, &block) || weavecRtIsStackOrGlobal(p))
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
  WEAVEC_RT_FORWARD(size, p);
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

void weavecRtShadowZero(unsigned char *start, size_t bytes) {
  const uintptr_t first = ((uintptr_t)start + pageBytes - 1) &
                          ~(uintptr_t)(pageBytes - 1);
  const uintptr_t last = ((uintptr_t)start + bytes) & ~(uintptr_t)(pageBytes - 1);
  if (last <= first ||
      mmap((void *)first, last - first, PROT_READ | PROT_WRITE,
           MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | MAP_NORESERVE, -1,
           0) == MAP_FAILED) {
    memset(start, 0, bytes);
    return;
  }
  memset(start, 0, first - (uintptr_t)start);
  memset((void *)last, 0, (uintptr_t)start + bytes - last);
}

/*===-- Lookup (section 3) --------------------------------------------------===*/

struct __weavec_rt_found __weavec_rt_find(const void *p) {
  const uintptr_t address = (uintptr_t)p;
  struct __weavec_rt_found found;
  WEAVEC_RT_FORWARD(find, p);
  found.state = WeavecRtUntracked;
  found.kind = WeavecRtHeap;
  found.base = 0;
  found.size = 0;
  found.frame = 0;
  found.scoped = 0;
  found.word = NULL;
  found.value = 0;
  weavecRtCount(WeavecRtStatLookups);
  if (inArena(address)) {
    weavecRtCount(WeavecRtStatHeapLookups);
    const SlotRef ref = slotOf(address);
    const uint32_t word = wordOf(ref);
    found.base = ref.base;
    if ((word & 3) == WeavecRtLive) {
      found.state = WeavecRtTrackedLive;
      found.size = word >> 2;
      found.word = &slotWords(ref.region)[ref.slot];
      found.value = word;
    } else {
      /* A free or never-allocated slot: a pointer into the arena that
       * points to no object is stale or wild. */
      found.state = WeavecRtTrackedDead;
    }
    return found;
  }
  {
    HugeBlock block;
    if (hugeFind(address, &block)) {
      found.base = block.base;
      found.size = block.size;
      found.state = block.dead != 0 || address < block.base ? WeavecRtTrackedDead
                                                      : WeavecRtTrackedLive;
      return found;
    }
  }
  return weavecRtFindStackOrGlobal(p);
}

/*===-- fork ----------------------------------------------------------------===*/

static void lockAll(void) {
  unsigned i;
  weavecRtLock(&initLock);
  for (i = 0; i < WeavecRtClassCount; ++i)
    weavecRtLock(&classState[i].lock);
  weavecRtLock(&hugeLock);
}

static void unlockAll(void) {
  unsigned i;
  weavecRtUnlock(&hugeLock);
  for (i = WeavecRtClassCount; i-- > 0;)
    weavecRtUnlock(&classState[i].lock);
  weavecRtUnlock(&initLock);
}

static void forkPrepare(void) { lockAll(); }
static void forkParent(void) { unlockAll(); }
static void forkChild(void) { unlockAll(); }

/*===-- The Darwin zone (section 2.5) ---------------------------------------===*/

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

static void zoneDestroy(malloc_zone_t *zone) { (void)zone; }

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
