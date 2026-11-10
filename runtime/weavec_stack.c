/*===- weavec_stack.c - Stacks: capture, the store, threads -------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0040, sections 1 and 2. One unwinder follows frame records
|* ([fp] the caller's fp, [fp + 8] the return address) within the calling
|* thread's stack, or the signal stack it runs on, dropping the runtime's
|* own frames at its start: a stack starts at the program's call into the
|* runtime. Unique stacks are kept once in a store mapped on first use and
|* never freed, so that a heap block names its allocation's and release's
|* stacks by two 32-bit identifiers (0: not recorded). Capture takes no lock,
|* allocates nothing and makes no system call after the first.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#if defined(__APPLE__)
#include <dlfcn.h>
#include <mach-o/getsect.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif

/*===-- Threads (section 5.1) -----------------------------------------------===*/

/* Threads in the order the runtime first sees them: the main thread, which
 * runs the first allocation and the constructors, is T0. On Darwin the
 * number is thread-specific data, not a thread-local variable: dyld makes a
 * thread's variables with calloc, whose capture would ask for them again. */
static unsigned nextThread;
#if defined(__APPLE__)
static pthread_key_t threadKey;
static int threadKeyReady;

unsigned weavecRtThread(void) {
  uintptr_t number;
  if (!__atomic_load_n(&threadKeyReady, __ATOMIC_ACQUIRE))
    return 0;
  number = (uintptr_t)pthread_getspecific(threadKey);
  if (number == 0) {
    number = __atomic_add_fetch(&nextThread, 1, __ATOMIC_RELAXED);
    (void)pthread_setspecific(threadKey, (void *)number);
  }
  return (unsigned)number - 1;
}

/* The calling thread's stack, from its thread structure. */
void weavecRtOwnStack(uintptr_t *low, uintptr_t *high) {
  pthread_t self = pthread_self();
  *high = (uintptr_t)pthread_get_stackaddr_np(self);
  *low = *high - pthread_get_stacksize_np(self);
}
#else
static __thread unsigned threadPlusOne;

unsigned weavecRtThread(void) {
  if (threadPlusOne == 0)
    threadPlusOne = __atomic_add_fetch(&nextThread, 1, __ATOMIC_RELAXED);
  return threadPlusOne - 1;
}

void weavecRtOwnStack(uintptr_t *low, uintptr_t *high) {
  *low = weavecRtStackBottom();
  *high = weavecRtStackTop();
}
#endif

/*===-- The runtime's code --------------------------------------------------===*/

/* The runtime's functions are in a section of their own (weavec_rt.h), so
 * that its frames are known in its library and in a program that links its
 * archive. */
static uintptr_t textLow;
static uintptr_t textHigh;
static int textKnown;

#if defined(__APPLE__)
static void findText(void) {
  Dl_info info;
  unsigned long size = 0;
  const uint8_t *start = NULL;
  if (dladdr((const void *)findText, &info) != 0 && info.dli_fbase != NULL)
    start = getsectiondata((const struct mach_header_64 *)info.dli_fbase,
                           "__TEXT", "__weavec_rt", &size);
  textLow = (uintptr_t)start;
  textHigh = start != NULL ? (uintptr_t)start + size : 0;
}
#else
extern const char __start_weavec_rt_text[]
    __attribute__((weak, visibility("hidden")));
extern const char __stop_weavec_rt_text[]
    __attribute__((weak, visibility("hidden")));

static void findText(void) {
  textLow = (uintptr_t)__start_weavec_rt_text;
  textHigh = (uintptr_t)__stop_weavec_rt_text;
}
#endif

static inline int inRuntime(uintptr_t pc) {
  if (!__atomic_load_n(&textKnown, __ATOMIC_ACQUIRE)) {
    findText();
    __atomic_store_n(&textKnown, 1, __ATOMIC_RELEASE);
  }
  return pc - textLow < textHigh - textLow;
}

/*===-- The unwinder (section 1) --------------------------------------------===*/

/* A return address without its pointer-authentication signature (arm64e's
 * code signs them; no user address has bits above 47). */
static inline uintptr_t stripped(uintptr_t address) {
#if defined(__aarch64__)
  return address & (((uintptr_t)1 << 48) - 1);
#else
  return address;
#endif
}

struct StackRange {
  uintptr_t low;
  uintptr_t high;
};

static int within(const struct StackRange *range, uintptr_t fp) {
  return range->high != 0 && fp >= range->low && fp <= range->high - 16;
}

static unsigned unwindIn(uintptr_t *frames, unsigned max, uintptr_t fp,
                         uintptr_t pc, uintptr_t low, uintptr_t high) {
  struct StackRange own = {low, high};
  struct StackRange signal = {0, 0};
  const struct StackRange *in = &own;
  unsigned count = 0;
  int skipping = pc == 0;
  if (pc != 0 && max != 0)
    frames[count++] = pc;
  /* A frame on the signal stack the thread runs on (a handler's). */
  if (!within(&own, fp)) {
    stack_t alternate;
    if (sigaltstack(NULL, &alternate) == 0 &&
        (alternate.ss_flags & SS_ONSTACK) != 0) {
      signal.low = (uintptr_t)alternate.ss_sp;
      signal.high = signal.low + alternate.ss_size;
    }
    in = &signal;
  }
  while (count < max && (fp & 7) == 0 && within(in, fp)) {
    const uintptr_t next = ((const uintptr_t *)fp)[0];
    const uintptr_t ret = stripped(((const uintptr_t *)fp)[1]);
    if (ret < 4096)
      break;
    if (!skipping || !inRuntime(ret)) {
      skipping = 0;
      /* The call's address, so that its line is the call's. */
      frames[count++] = ret - 1;
    }
    /* Frame records go up the stack, and leave a signal stack for the
     * interrupted thread's own. */
    if (in == &signal && !within(&signal, next) && within(&own, next))
      in = &own;
    else if (next <= fp)
      break;
    fp = next;
  }
  return count;
}

unsigned weavecRtUnwind(uintptr_t *frames, unsigned max, uintptr_t fp,
                        uintptr_t pc) {
  uintptr_t low;
  uintptr_t high;
  weavecRtOwnStack(&low, &high);
  return unwindIn(frames, max, fp, pc, low, high);
}

/*===-- The store (section 2) -----------------------------------------------===*/

enum {
  StoreSlots = 1 << 20,
  StoreBytes = 256 << 20,
  /* Probes of the table before a capture gives up (the store is full). */
  StoreProbes = 128
};

struct Stored {
  uint64_t hash;
  uint32_t count;
  uint32_t thread;
  uintptr_t frames[];
};

/* 0: not mapped; 1: being mapped; 2: ready; 3: failed. */
static int storeState;
/* Entry i: the offset of its stack in the area, in words, plus 1. */
static uint32_t *storeTable;
static unsigned char *storeArea;
static size_t storeUsed;
static size_t storeEntries;

unsigned weavecRtHeapStacks = WeavecRtHeapFrames;

/* WEAVEC_RT_HEAP_STACKS=<frames> (0: none), read before the program's
 * constructors. */
__attribute__((constructor(101))) static void readStackSettings(void) {
  const char *value = getenv("WEAVEC_RT_HEAP_STACKS");
#if defined(__APPLE__)
  if (pthread_key_create(&threadKey, NULL) == 0)
    __atomic_store_n(&threadKeyReady, 1, __ATOMIC_RELEASE);
#endif
  (void)weavecRtThread();
  if (value != NULL && *value >= '0' && *value <= '9') {
    const unsigned long frames = strtoul(value, NULL, 10);
    weavecRtHeapStacks = frames < WeavecRtHeapFramesMax ? (unsigned)frames
                                                        : WeavecRtHeapFramesMax;
  }
  (void)inRuntime(0);
}

static void *mapStore(size_t bytes) {
  void *map = mmap(NULL, bytes, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
  if (map == MAP_FAILED)
    return NULL;
  weavecRtExcluded(map, bytes);
  return map;
}

static int storeReady(void) {
  int state = __atomic_load_n(&storeState, __ATOMIC_ACQUIRE);
  if (state == 2)
    return 1;
  if (state != 0 || !__atomic_compare_exchange_n(&storeState, &state, 1, 0,
                                                 __ATOMIC_ACQUIRE,
                                                 __ATOMIC_RELAXED))
    return 0;
  storeTable = (uint32_t *)mapStore(StoreSlots * sizeof(uint32_t));
  storeArea = storeTable != NULL ? (unsigned char *)mapStore(StoreBytes) : NULL;
  __atomic_store_n(&storeState, storeArea != NULL ? 2 : 3, __ATOMIC_RELEASE);
  return storeArea != NULL;
}

/* (Each frame's product is independent of the others', so that a capture
 * does not wait on a chain of multiplications; the end mixes them.) */
static uint64_t hashStack(const uintptr_t *frames, unsigned count,
                          unsigned thread) {
  uint64_t hash = ((uint64_t)thread << 32 | count) * 0x9E3779B97F4A7C15ULL;
  unsigned i;
  for (i = 0; i < count; ++i)
    hash += (frames[i] ^ ((uint64_t)i << 56)) * 0xC2B2AE3D27D4EB4FULL;
  hash ^= hash >> 31;
  hash *= 0xBF58476D1CE4E5B9ULL;
  hash ^= hash >> 29;
  return hash != 0 ? hash : 1;
}

static int sameStack(const struct Stored *stored, uint64_t hash,
                     const uintptr_t *frames, unsigned count,
                     unsigned thread) {
  return stored->hash == hash && stored->count == count &&
         stored->thread == thread &&
         memcmp(stored->frames, frames, count * sizeof(uintptr_t)) == 0;
}

/* The identifiers of recently stored stacks, by hash, shared by the
 * threads: an allocation site repeats its stacks, and the store's table and
 * records are far apart in memory. An entry is the hash's high 44 bits and
 * the identifier's index, so that a read is never torn; with the 12 bits
 * that choose the entry, 56 bits of the hash must match. */
enum { RecentBits = 12, IndexBits = 20 };
static uint64_t recent[1 << RecentBits];

uint32_t weavecRtStackStore(const uintptr_t *frames, unsigned count,
                            unsigned thread) {
  const uint64_t hash = hashStack(frames, count, thread);
  uint64_t *cached = &recent[hash & ((1 << RecentBits) - 1)];
  const uint64_t seen = __atomic_load_n(cached, __ATOMIC_RELAXED);
  uint32_t index = (uint32_t)(hash ^ (hash >> 32)) & (StoreSlots - 1);
  unsigned probe;
  if (count == 0)
    return 0;
  if (seen != 0 && (seen >> IndexBits) == (hash >> IndexBits))
    return (uint32_t)(seen & ((1 << IndexBits) - 1)) + 1;
  if (!storeReady())
    return 0;
  for (probe = 0; probe < StoreProbes; ++probe) {
    uint32_t entry = __atomic_load_n(&storeTable[index], __ATOMIC_ACQUIRE);
    if (entry == 0) {
      const size_t bytes = sizeof(struct Stored) + count * sizeof(uintptr_t);
      const size_t at = __atomic_fetch_add(&storeUsed, bytes, __ATOMIC_RELAXED);
      struct Stored *stored = (struct Stored *)(storeArea + at);
      if (at + bytes > StoreBytes)
        return 0;
      stored->hash = hash;
      stored->count = count;
      stored->thread = thread;
      memcpy(stored->frames, frames, count * sizeof(uintptr_t));
      /* Two captures of one new stack may both store it (harmlessly): the
       * first claim wins. */
      if (__atomic_compare_exchange_n(&storeTable[index], &entry,
                                      (uint32_t)(at / 8) + 1, 0,
                                      __ATOMIC_RELEASE, __ATOMIC_ACQUIRE)) {
        __atomic_add_fetch(&storeEntries, 1, __ATOMIC_RELAXED);
        break;
      }
    }
    if (sameStack((const struct Stored *)(storeArea + (size_t)(entry - 1) * 8),
                  hash, frames, count, thread))
      break;
    index = (index + 1) & (StoreSlots - 1);
  }
  if (probe == StoreProbes)
    return 0;
  __atomic_store_n(cached, (hash >> IndexBits) << IndexBits | index,
                   __ATOMIC_RELAXED);
  return index + 1;
}

unsigned weavecRtStackGet(uint32_t id, uintptr_t *frames, unsigned max,
                          unsigned *thread) {
  uint32_t entry;
  const struct Stored *stored;
  unsigned count;
  if (id == 0 || id > StoreSlots ||
      __atomic_load_n(&storeState, __ATOMIC_ACQUIRE) != 2)
    return 0;
  entry = __atomic_load_n(&storeTable[id - 1], __ATOMIC_ACQUIRE);
  if (entry == 0)
    return 0;
  stored = (const struct Stored *)(storeArea + (size_t)(entry - 1) * 8);
  count = stored->count < max ? stored->count : max;
  memcpy(frames, stored->frames, count * sizeof(uintptr_t));
  *thread = stored->thread;
  return count;
}

/* A heap block's stack: the unwinder's walk, lean, for every allocation and
 * release (no signal stack; the runtime's code bounds read once). */
uint32_t weavecRtStackId(uintptr_t fp, uintptr_t low, uintptr_t high,
                         unsigned thread) {
  uintptr_t frames[WeavecRtHeapFramesMax];
  const unsigned max = weavecRtHeapStacks;
  unsigned count = 0;
  while (count < max && (fp & 7) == 0 && fp >= low && fp + 16 <= high) {
    const uintptr_t next = ((const uintptr_t *)fp)[0];
    const uintptr_t ret = stripped(((const uintptr_t *)fp)[1]);
    if (ret < 4096)
      break;
    if (count != 0 || !inRuntime(ret))
      frames[count++] = ret - 1;
    if (next <= fp)
      break;
    fp = next;
  }
  return weavecRtStackStore(frames, count, thread);
}

void weavecRtStoreStats(size_t *entries, size_t *bytes) {
  *entries = __atomic_load_n(&storeEntries, __ATOMIC_RELAXED);
  *bytes = __atomic_load_n(&storeUsed, __ATOMIC_RELAXED);
  if (*bytes > StoreBytes)
    *bytes = StoreBytes;
}
