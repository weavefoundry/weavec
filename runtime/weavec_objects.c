/*===- weavec_objects.c - Stack and global objects, and the guards -*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0032, sections 3 to 5.
|*
|* Stack objects are the locals a compiled function registers when it
|* declares them and unregisters when their scope ends. They are kept per
|* thread, ordered by the frame that registered them (deepest last). A
|* `longjmp` skips the unregistration, so an entry is trusted only while its
|* frame can still be live: entries of frames deeper than the one entering,
|* leaving, rewinding or looking up are dropped.
|*
|* Global objects come from the descriptors each unit emits into one
|* section; every image hands its section to `__weavec_rt_globals_add` from
|* a constructor.
|*
|* The guards answer 0 (pass) or 1 (fail); the check prelude turns a failure
|* into a trap or a report at the access.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <pthread.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/*===-- Stack objects (section 4.3) -----------------------------------------===*/

typedef struct {
  uintptr_t base;
  size_t size;
  uintptr_t frame;
  /* WeavecRtLoose, WeavecRtScoped. */
  int flags;
} StackEntry;

enum { InlineEntries = 32 };

typedef struct {
  StackEntry *entries;
  size_t count;
  size_t capacity;
  /* The thread's stack: [low, high). Both 0 when unknown. */
  uintptr_t low;
  uintptr_t high;
  int ready;
  StackEntry inlineEntries[InlineEntries];
} StackList;

static __thread StackList stackList;

static pthread_key_t stackKey;
static WeavecRtLock stackKeyLock;
static int stackKeyReady;

static void releaseStackList(void *start) {
  /* The size is in the mapping's first word. */
  const size_t bytes = *(const size_t *)start;
  (void)munmap(start, bytes);
}

static void readStackBounds(StackList *list) {
  pthread_t self = pthread_self();
  list->entries = list->inlineEntries;
  list->capacity = InlineEntries;
  list->ready = 1;
  (void)weavecRtInitialise();
#if defined(__APPLE__)
  {
    const uintptr_t top = (uintptr_t)pthread_get_stackaddr_np(self);
    const size_t size = pthread_get_stacksize_np(self);
    if (top != 0 && size != 0 && size < top) {
      list->high = top;
      list->low = top - size;
    }
  }
#elif defined(__linux__)
  {
    pthread_attr_t attributes;
    void *low = NULL;
    size_t size = 0;
    if (pthread_getattr_np(self, &attributes) == 0) {
      if (pthread_attr_getstack(&attributes, &low, &size) == 0 && low != NULL) {
        list->low = (uintptr_t)low;
        list->high = (uintptr_t)low + size;
      }
      (void)pthread_attr_destroy(&attributes);
    }
  }
#else
  (void)self;
#endif
  /* RFC 0034, section 2.1: a thread's stack may be an exited thread's,
   * whose objects a `pthread_exit` or `longjmp` left in the shadow. Below
   * the calling frame nothing is live yet. */
  if (list->low != 0)
    weavecRtShadowClear(list->low, (uintptr_t)__builtin_frame_address(0));
}

static inline StackList *threadList(void) {
  StackList *list = &stackList;
  if (__builtin_expect(!list->ready, 0))
    readStackBounds(list);
  return list;
}

/* A grown list's mapping: its size, then the entries. */
typedef struct {
  size_t bytes;
  size_t pad;
  StackEntry entries[];
} StackMapping;

static int growStackList(StackList *list) {
  const size_t capacity = list->capacity * 2;
  const size_t bytes =
      offsetof(StackMapping, entries) + capacity * sizeof(StackEntry);
  StackMapping *mapping = (StackMapping *)mmap(
      NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ((void *)mapping == MAP_FAILED)
    return 0;
  mapping->bytes = bytes;
  memcpy(mapping->entries, list->entries, list->count * sizeof(StackEntry));
  if (list->entries != list->inlineEntries)
    releaseStackList((char *)list->entries - offsetof(StackMapping, entries));
  list->entries = mapping->entries;
  list->capacity = capacity;
  /* The mapping goes when the thread does. */
  weavecRtLock(&stackKeyLock);
  if (!stackKeyReady && pthread_key_create(&stackKey, releaseStackList) == 0)
    stackKeyReady = 1;
  weavecRtUnlock(&stackKeyLock);
  if (stackKeyReady)
    (void)pthread_setspecific(stackKey, mapping);
  return 1;
}

/* Drops the entries from `count` on, and their shadow tags. */
static inline void truncateList(StackList *list, size_t count) {
  while (list->count > count) {
    const StackEntry *entry = &list->entries[--list->count];
    weavecRtShadowForget(entry->base, entry->size);
  }
}

/* Drops the entries of frames deeper than `frame`: those frames are gone. */
static inline void dropDeeper(StackList *list, uintptr_t frame) {
  size_t count = list->count;
  while (count != 0 && list->entries[count - 1].frame < frame)
    --count;
  truncateList(list, count);
}

void *__weavec_rt_stack_enter(void *base, size_t size, void *framePointer,
                              int flags) {
  WEAVEC_RT_FORWARD(stackEnter, base, size, framePointer, flags);
  StackList *list = threadList();
  if (__builtin_expect(!weavecRtShadowReady(), 0))
    (void)weavecRtInitialise();
  const uintptr_t frame = (uintptr_t)framePointer;
  const uintptr_t start = (uintptr_t)base;
  size_t i;
  /* A coroutine's or a signal's stack: not this list's. */
  if (frame < list->low || frame >= list->high)
    return base;
  dropDeeper(list, frame);
  /* An entry of this frame address that overlaps the new object is left
   * from an earlier activation. */
  for (i = list->count; i-- > 0 && list->entries[i].frame == frame;) {
    const StackEntry *entry = &list->entries[i];
    if (entry->base < start + size && start < entry->base + entry->size) {
      weavecRtShadowForget(entry->base, entry->size);
      memmove(&list->entries[i], &list->entries[i + 1],
              (list->count - i - 1) * sizeof(StackEntry));
      --list->count;
    }
  }
  weavecRtCount(WeavecRtStatStackEnters);
  if (list->count == list->capacity && !growStackList(list))
    return base;
  list->entries[list->count].base = start;
  list->entries[list->count].size = size;
  list->entries[list->count].frame = frame;
  list->entries[list->count].flags = flags;
  ++list->count;
  weavecRtShadowObject(start, size, (flags & WeavecRtLoose) == 0);
  return base;
}

void __weavec_rt_stack_leave(void *base, void *framePointer) {
  WEAVEC_RT_FORWARD_VOID(stackLeave, base, framePointer);
  StackList *list = threadList();
  const uintptr_t frame = (uintptr_t)framePointer;
  size_t i;
  dropDeeper(list, frame);
  for (i = list->count; i-- > 0 && list->entries[i].frame == frame;)
    if (list->entries[i].base == (uintptr_t)base) {
      /* It and whatever was declared after it in the frame. */
      truncateList(list, i);
      return;
    }
}

void __weavec_rt_stack_rewind(void *frame) {
  StackList *list;
  WEAVEC_RT_FORWARD_VOID(stackRewind, frame);
  list = threadList();
  dropDeeper(list, (uintptr_t)frame);
  /* The frames a `longjmp` abandoned lie below `frame`, whose function
   * registers nothing (it calls setjmp); the objects the compiler entered
   * inline there are only in the shadow (RFC 0034, section 4). */
  if (list->low != 0 && (uintptr_t)frame > list->low &&
      (uintptr_t)frame <= list->high)
    weavecRtShadowClear(list->low, (uintptr_t)frame);
}

/* Whether the frame has automatic storage the list does not know: any of
 * its entries says so. A function inlined into another shares its frame, so
 * the answer is the frame's, not the entry's. */
static int frameIsLoose(const StackList *list, uintptr_t frame, size_t from,
                        size_t to) {
  size_t i;
  for (i = from; i < to; ++i)
    if (list->entries[i].frame == frame &&
        (list->entries[i].flags & WeavecRtLoose) != 0)
      return 1;
  return 0;
}

/* The stack object `address` points into. An address that is one past the
 * end of an object, where no object starts, still belongs to it (unless the
 * object is loose): a walk off its end is caught at the first step. */
static int findStack(uintptr_t address, struct __weavec_rt_found *found) {
  StackList *list = threadList();
  const StackEntry *inside = NULL;
  const StackEntry *ending = NULL;
  size_t low = 0;
  size_t high;
  size_t first;
  size_t i;
  if (address < list->low || address >= list->high)
    return 0;
  /* This function's own frame is deeper than every live one. */
  dropDeeper(list, (uintptr_t)__builtin_frame_address(0));
  /* Frames nest, so an object lies between the frame addresses of the next
   * deeper and the next shallower group: it is in the last group whose frame
   * is above the address, or in the one after it. */
  high = list->count;
  while (low < high) {
    const size_t mid = low + (high - low) / 2;
    if (list->entries[mid].frame > address)
      low = mid + 1;
    else
      high = mid;
  }
  /* `low` entries have a frame above the address. */
  i = low;
  if (i < list->count) {
    const uintptr_t frame = list->entries[i].frame;
    while (i < list->count && list->entries[i].frame == frame)
      ++i;
  }
  high = i;
  i = low;
  if (i != 0) {
    const uintptr_t frame = list->entries[i - 1].frame;
    while (i != 0 && list->entries[i - 1].frame == frame)
      --i;
  }
  first = i;
  for (; i < high; ++i) {
    const StackEntry *entry = &list->entries[i];
    if (address >= entry->base && address - entry->base < entry->size)
      inside = entry;
    else if (address == entry->base + entry->size)
      ending = entry;
    else if (entry->size == 0 && address == entry->base)
      ending = entry;
  }
  if (inside == NULL && ending != NULL &&
      !frameIsLoose(list, ending->frame, first, high))
    inside = ending;
  if (inside == NULL)
    return 0;
  found->state = WeavecRtTrackedLive;
  found->kind = WeavecRtStack;
  found->base = inside->base;
  found->size = inside->size;
  found->frame = inside->frame;
  found->scoped = (inside->flags & WeavecRtScoped) != 0;
  return 1;
}

/*===-- Global objects (section 5) ------------------------------------------===*/

typedef struct {
  uintptr_t base;
  size_t size;
} GlobalEntry;

/* The table of global objects, sorted by address. Lookups read it without a
 * lock, so that a guard in a signal handler that interrupted a lookup still
 * finds them: writers (the constructors that add a unit's globals) hold
 * `globalsLock`, make `globalsSequence` odd while they change the table and
 * even again after, and a reader retries a read that overlapped a change. A
 * grown table is never unmapped, since a reader may still be in it. */
typedef struct {
  size_t capacity;
  size_t count;
  GlobalEntry entries[];
} GlobalTable;

static WeavecRtLock globalsLock;
static GlobalTable *globalTable;
static unsigned long globalsSequence;
static uintptr_t globalsLow = ~(uintptr_t)0;
static uintptr_t globalsHigh;

static void siftDown(GlobalEntry *entries, size_t root, size_t count) {
  for (;;) {
    size_t child = 2 * root + 1;
    GlobalEntry swap;
    if (child >= count)
      return;
    if (child + 1 < count && entries[child].base < entries[child + 1].base)
      ++child;
    if (entries[root].base >= entries[child].base)
      return;
    swap = entries[root];
    entries[root] = entries[child];
    entries[child] = swap;
    root = child;
  }
}

static void sortEntries(GlobalEntry *entries, size_t count) {
  size_t i;
  for (i = count / 2; i-- > 0;)
    siftDown(entries, i, count);
  for (i = count; i-- > 1;) {
    const GlobalEntry swap = entries[0];
    entries[0] = entries[i];
    entries[i] = swap;
    siftDown(entries, 0, i);
  }
}

/* A table with room for `capacity` entries holding `table`'s. */
static GlobalTable *growGlobals(const GlobalTable *table, size_t capacity) {
  const size_t count = table != NULL ? table->count : 0;
  GlobalTable *grown = (GlobalTable *)mmap(
      NULL, offsetof(GlobalTable, entries) + capacity * sizeof(GlobalEntry),
      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if ((void *)grown == MAP_FAILED)
    return NULL;
  grown->capacity = capacity;
  grown->count = count;
  if (count != 0)
    memcpy(grown->entries, table->entries, count * sizeof(GlobalEntry));
  return grown;
}

void __weavec_rt_globals_add(const void *const *begin, const void *const *end) {
  WEAVEC_RT_FORWARD_VOID(globalsAdd, begin, end);
  const size_t pairs = (size_t)(end - begin) / 2;
  GlobalTable *table;
  GlobalEntry *added;
  size_t count;
  size_t fresh = 0;
  size_t i;
  size_t j;
  size_t kept = 0;
  if (begin == NULL || pairs == 0)
    return;
  /* The shadow first: the globals' tags go there (RFC 0034, section 2.1). */
  (void)weavecRtInitialise();
  weavecRtLock(&globalsLock);
  table = globalTable;
  count = table != NULL ? table->count : 0;
  /* Room for the new entries twice: sorted at the end, then merged. */
  if (table == NULL || count + 2 * pairs > table->capacity) {
    size_t capacity = table != NULL ? table->capacity : 1024;
    while (capacity < count + 2 * pairs)
      capacity *= 2;
    table = growGlobals(table, capacity);
    if (table == NULL) {
      weavecRtUnlock(&globalsLock);
      return;
    }
    __atomic_store_n(&globalTable, table, __ATOMIC_RELEASE);
  }
  __atomic_store_n(&globalsSequence, globalsSequence + 1, __ATOMIC_RELAXED);
  __atomic_thread_fence(__ATOMIC_RELEASE);
  added = table->entries + table->capacity - pairs;
  for (i = 0; i < pairs; ++i) {
    const uintptr_t base = (uintptr_t)begin[2 * i];
    const size_t size = (size_t)(uintptr_t)begin[2 * i + 1];
    if (base == 0)
      continue;
    added[fresh].base = base;
    added[fresh].size = size;
    ++fresh;
    if (base < __atomic_load_n(&globalsLow, __ATOMIC_RELAXED))
      __atomic_store_n(&globalsLow, base, __ATOMIC_RELAXED);
    if (base + size + 1 > __atomic_load_n(&globalsHigh, __ATOMIC_RELAXED))
      __atomic_store_n(&globalsHigh, base + size + 1, __ATOMIC_RELAXED);
  }
  sortEntries(added, fresh);
  /* Merge from the end: the next write is never past an unread entry. */
  i = count;
  j = fresh;
  while (j != 0) {
    if (i != 0 && table->entries[i - 1].base > added[j - 1].base) {
      table->entries[i + j - 1] = table->entries[i - 1];
      --i;
    } else {
      table->entries[i + j - 1] = added[j - 1];
      --j;
    }
  }
  /* One address's descriptors merge to the largest size (tentative
   * definitions of different sizes). */
  for (i = 0; i < count + fresh; ++i) {
    if (kept != 0 && table->entries[kept - 1].base == table->entries[i].base) {
      if (table->entries[i].size > table->entries[kept - 1].size)
        table->entries[kept - 1].size = table->entries[i].size;
      continue;
    }
    table->entries[kept++] = table->entries[i];
  }
  table->count = kept;
  /* RFC 0034, section 2.1. */
  for (i = 0; i < kept; ++i)
    weavecRtShadowObject(table->entries[i].base, table->entries[i].size, 1);
  __atomic_store_n(&globalsSequence, globalsSequence + 1, __ATOMIC_RELEASE);
  weavecRtUnlock(&globalsLock);
}

/* The global objects around `address`: the last one starting at or below it
 * (`below`) and the first one above it (`above`); 0 for none. Fails if a
 * change it interrupted never finishes. */
static int globalsAround(uintptr_t address, GlobalEntry *below,
                          GlobalEntry *above) {
  unsigned tries;
  for (tries = 0; tries < 1u << 16; ++tries) {
    const unsigned long sequence =
        __atomic_load_n(&globalsSequence, __ATOMIC_ACQUIRE);
    const GlobalTable *table;
    size_t low = 0;
    size_t high;
    size_t count;
    below->base = 0;
    below->size = 0;
    above->base = 0;
    above->size = 0;
    if (sequence & 1)
      continue;
    table = __atomic_load_n(&globalTable, __ATOMIC_ACQUIRE);
    if (table == NULL)
      return 1;
    count = __atomic_load_n(&table->count, __ATOMIC_RELAXED);
    if (count > table->capacity)
      count = table->capacity;
    high = count;
    while (low < high) {
      const size_t mid = low + (high - low) / 2;
      if (__atomic_load_n(&table->entries[mid].base, __ATOMIC_RELAXED) <=
          address)
        low = mid + 1;
      else
        high = mid;
    }
    /* `low` entries start at or below the address. */
    if (low != 0) {
      below->base = __atomic_load_n(&table->entries[low - 1].base,
                                    __ATOMIC_RELAXED);
      below->size = __atomic_load_n(&table->entries[low - 1].size,
                                    __ATOMIC_RELAXED);
    }
    if (low < count) {
      above->base =
          __atomic_load_n(&table->entries[low].base, __ATOMIC_RELAXED);
      above->size =
          __atomic_load_n(&table->entries[low].size, __ATOMIC_RELAXED);
    }
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (__atomic_load_n(&globalsSequence, __ATOMIC_RELAXED) == sequence)
      return 1;
  }
  return 0;
}

/* The global object `address` points into. */
static int findGlobal(uintptr_t address, struct __weavec_rt_found *found) {
  GlobalEntry below;
  GlobalEntry above;
  if (address < __atomic_load_n(&globalsLow, __ATOMIC_RELAXED) ||
      address >= __atomic_load_n(&globalsHigh, __ATOMIC_RELAXED))
    return 0;
  if (!globalsAround(address, &below, &above) || below.base == 0 || address - below.base >= below.size)
    return 0;
  found->base = below.base;
  found->size = below.size;
  found->state = WeavecRtTrackedLive;
  found->kind = WeavecRtGlobal;
  return 1;
}

/*===-- Lookup --------------------------------------------------------------===*/

/* RFC 0034, section 4: a stack or global object the shadow encodes exactly
 * (the compiler entered it inline, or a global on a granule). An address
 * one past its end, where no other object starts, still belongs to it. */
static int findExact(uintptr_t address, struct __weavec_rt_found *found) {
  uintptr_t end = weavecRtShadowObjectEnd(address);
  uintptr_t first;
  if (end == 0 && address != 0) {
    end = weavecRtShadowObjectEnd(address - 1);
    if (end != address)
      return 0;
    address -= 1;
  }
  if (end == 0)
    return 0;
  /* Back to its first granule (the slow path only): the granules with its
   * end are one run, found by doubling steps back and then halving them. */
  first = address >> 4;
  {
    uintptr_t step = 1;
    while (step <= first &&
           weavecRtShadowObjectEnd((first - step) << 4) == end) {
      first -= step;
      step <<= 1;
    }
    for (; step != 0; step >>= 1)
      if (step <= first &&
          weavecRtShadowObjectEnd((first - step) << 4) == end)
        first -= step;
  }
  found->state = WeavecRtTrackedLive;
  found->kind = WeavecRtStack;
  found->base = first << 4;
  found->size = end - (first << 4);
  return 1;
}

struct __weavec_rt_found weavecRtFindStackOrGlobal(const void *p) {
  struct __weavec_rt_found found;
  found.state = WeavecRtUntracked;
  found.kind = WeavecRtHeap;
  found.base = 0;
  found.size = 0;
  found.frame = 0;
  found.scoped = 0;
  found.word = NULL;
  found.value = 0;
  /* (The globals' table first: decoding a large global's start from the
   * shadow walks it backwards.) */
  if (findGlobal((uintptr_t)p, &found))
    weavecRtCount(WeavecRtStatGlobalLookups);
  else if (findStack((uintptr_t)p, &found) || findExact((uintptr_t)p, &found))
    weavecRtCount(WeavecRtStatStackLookups);
  else
    weavecRtCount(WeavecRtStatUntrackedLookups);
  return found;
}

int weavecRtIsStackOrGlobal(const void *p) {
  const StackList *list = threadList();
  const uintptr_t address = (uintptr_t)p;
  struct __weavec_rt_found found;
  if (findGlobal(address, &found))
    return 1;
  /* Anything in the live part of the calling thread's stack. */
  if (address >= (uintptr_t)__builtin_frame_address(0) && address < list->high)
    return 1;
  return findExact(address, &found);
}

/*===-- What the shadow answers (RFC 0034, section 2.5) ---------------------===*/

static inline int inArena(uintptr_t address) {
  return address - __weavec_rt_heap.base < __weavec_rt_heap.bytes;
}

static inline int isHeapLive(unsigned char v) {
  return (v >= 1 && v <= WeavecRtShadowWhole) || v == WeavecRtShadowTail;
}

/* 1 when the shadow shows that `address` is in no live tracked object and
 * that no lookup is needed to say so: untracked (outside the arena, where
 * every tracked object has a non-zero byte) or a dead arena slot. */
static inline int shadowNotLive(uintptr_t address) {
  return *weavecRtShadowOf(address) == WeavecRtShadowDead;
}

/* Whether the shadow alone shows that the `width` bytes at `at`, reached
 * from `from`, pass RFC 0033 section 4's rule: 1 pass, 0 fail, -1 ask the
 * lookup. */
static int shadowObject(uintptr_t from, uintptr_t at, unsigned long long width) {
  uintptr_t last;
  uintptr_t g;
  unsigned char first;
  if (!weavecRtShadowReady() || width == 0 ||
      __builtin_add_overflow(at, (uintptr_t)(width - 1), &last))
    return -1;
  first = *weavecRtShadowOf(at);
  if (isHeapLive(first)) {
    /* Bytes inside one live heap object pass wherever `from` is. Beyond a
     * few granules the slot's lookup (constant time) is cheaper than the
     * walk (a `memcmp` of a long run that stops at its first byte). */
    if ((last >> 4) - (at >> 4) > 3)
      return -1;
    for (g = at >> 4; g <= last >> 4; ++g) {
      const unsigned char v = *weavecRtShadowOf(g << 4);
      const unsigned need = g == (last >> 4) ? (unsigned)(last & 15) + 1 : 16;
      if (v < 1 || v > WeavecRtShadowWhole || v < need)
        return -1;
    }
    return 1;
  }
  if (weavecRtIsObjectByte(first)) {
    /* One stack or global object's bytes, reached from inside it or from
     * no stack or global object; an access past its end fails. */
    const unsigned char origin = *weavecRtShadowOf(from);
    const uintptr_t end = weavecRtShadowObjectEnd(at);
    if (end == 0)
      return -1;
    if (at + width > end || at + width < at)
      return 0;
    if ((weavecRtIsObjectByte(origin) && weavecRtShadowObjectEnd(from) == end &&
         from < end) ||
        isHeapLive(origin) || origin == WeavecRtShadowDead)
      return 1;
    /* From inside another one: forwards fails, backwards passes. */
    if (weavecRtIsObjectByte(origin))
      return at > from ? 0 : 1;
    return -1;
  }
  if (first == WeavecRtShadowDead && !inArena(at)) {
    /* Untracked bytes, reached from no live tracked object. */
    for (g = at >> 4; g <= last >> 4; ++g)
      if (*weavecRtShadowOf(g << 4) != WeavecRtShadowDead || inArena(g << 4))
        return -1;
    return from == at || shadowNotLive(from) ? 1 : -1;
  }
  return -1;
}

/* The same for a `live` guard of `p`. */
static int shadowLive(uintptr_t p) {
  unsigned char v;
  if (!weavecRtShadowReady())
    return -1;
  v = *weavecRtShadowOf(p);
  if (isHeapLive(v) || weavecRtIsObjectByte(v) ||
      (v == WeavecRtShadowDead && !inArena(p)))
    return 1;
  return -1;
}

/*===-- Guards (section 3) --------------------------------------------------===*/

/* RFC 0033 section 4: the address of the access `p + offset + index * step`,
 * or 0 when the arithmetic leaves the address space. */
static int accessAddress(const void *p, long long index,
                         unsigned long long step, unsigned long long offset,
                         uintptr_t *address) {
  long long stride;
  uintptr_t at;
  if (step > (unsigned long long)__LONG_LONG_MAX__ ||
      __builtin_mul_overflow(index, (long long)step, &stride) ||
      __builtin_add_overflow((uintptr_t)p, (uintptr_t)offset, &at))
    return 0;
  if (stride >= 0 ? __builtin_add_overflow(at, (uintptr_t)stride, &at)
                  : __builtin_sub_overflow(at, (uintptr_t)0 - (uintptr_t)stride,
                                           &at))
    return 0;
  *address = at;
  return 1;
}

/* RFC 0033 section 4: whether the `width` bytes at `address`, reached from
 * the pointer `from`, fail, given the object `found` that holds the first of
 * them. They pass inside one live heap object, wherever `from` points (an
 * index may bring a pointer formed outside a buffer back into it). Inside a
 * live stack or global object they fail when reached forwards from inside
 * another one: such objects lie next to each other with no gap, and a walk
 * off one lands in the next. Outside every object they pass unless their
 * last byte is in one, or `from` is in a live object (the access left it for
 * memory nothing tracks). */
static int accessFails(const struct __weavec_rt_found *found,
                       uintptr_t address, unsigned long long width,
                       const void *from) {
  struct __weavec_rt_found origin;
  if (found->state == WeavecRtTrackedDead)
    return 1;
  if (found->state == WeavecRtTrackedLive) {
    if (width > found->size ||
        (unsigned long long)(address - found->base) > found->size - width)
      return 1;
    if (found->kind == WeavecRtHeap || address <= (uintptr_t)from ||
        (uintptr_t)from - found->base < found->size)
      return 0;
    origin = __weavec_rt_find(from);
    return origin.state == WeavecRtTrackedLive && origin.kind != WeavecRtHeap &&
           origin.base != found->base;
  }
  if (width > 1 &&
      __weavec_rt_find((const void *)(address + (width - 1))).state !=
          WeavecRtUntracked)
    return 1;
  return (uintptr_t)from != address &&
         __weavec_rt_find(from).state == WeavecRtTrackedLive;
}

int __weavec_rt_object(const void *p, long long index, unsigned long long step,
                       unsigned long long offset, unsigned long long width) {
  WEAVEC_RT_FORWARD(object, p, index, step, offset, width);
  uintptr_t address;
  struct __weavec_rt_found found;
  if (!accessAddress(p, index, step, offset, &address))
    return 1;
  {
    const int answer = shadowObject((uintptr_t)p, address, width);
    if (answer >= 0)
      return !answer;
  }
  found = __weavec_rt_find((const void *)address);
  return accessFails(&found, address, width, p);
}

int __weavec_rt_string(const char *p) {
  WEAVEC_RT_FORWARD(string, p);
  const struct __weavec_rt_found found = __weavec_rt_find(p);
  uintptr_t at;
  if (found.state == WeavecRtUntracked)
    return 0;
  if (found.state == WeavecRtTrackedDead)
    return 1;
  at = (uintptr_t)p - found.base;
  if (at >= found.size)
    return 1;
  return memchr(p, 0, found.size - at) == NULL;
}

unsigned long long __weavec_rt_strlen(const char *p) {
  WEAVEC_RT_FORWARD(strlen, p);
  const struct __weavec_rt_found found = __weavec_rt_find(p);
  const char *end;
  uintptr_t at;
  if (found.state == WeavecRtUntracked)
    return strlen(p);
  if (found.state == WeavecRtTrackedDead)
    return ~0ULL;
  at = (uintptr_t)p - found.base;
  if (at >= found.size)
    return ~0ULL;
  end = (const char *)memchr(p, 0, found.size - at);
  return end != NULL ? (unsigned long long)(end - p) : ~0ULL;
}

unsigned long long __weavec_rt_room(const void *p) {
  WEAVEC_RT_FORWARD(room, p);
  const struct __weavec_rt_found found = __weavec_rt_find(p);
  uintptr_t at;
  if (found.state == WeavecRtUntracked)
    return ~0ULL;
  if (found.state == WeavecRtTrackedDead)
    return 0;
  at = (uintptr_t)p - found.base;
  return at < found.size ? found.size - at : 0;
}

int __weavec_rt_live(const void *p) {
  WEAVEC_RT_FORWARD(live, p);
  return __weavec_rt_find(p).state == WeavecRtTrackedDead;
}

/* RFC 0034, section 2.5. */
static const char *guardName(unsigned kind) {
  return (kind & WeavecRtGuardKindMask) == WeavecRtGuardLive ? "live"
                                                             : "object";
}

void __weavec_rt_guard(const void *from, const void *at,
                       unsigned long long width, unsigned kind,
                       const struct __weavec_rt_site *site) {
  WEAVEC_RT_FORWARD_VOID(guard, from, at, width, kind, site);
  int failed;
  weavecRtCount(WeavecRtStatSlowGuards);
  if ((kind & WeavecRtGuardOverflow) != 0) {
    failed = 1;
  } else if ((kind & WeavecRtGuardKindMask) == WeavecRtGuardLive) {
    if (shadowLive((uintptr_t)from) == 1)
      return;
    failed = __weavec_rt_find(from).state == WeavecRtTrackedDead;
  } else {
    const int answer = shadowObject((uintptr_t)from, (uintptr_t)at, width);
    if (answer == 1)
      return;
    if (answer == 0) {
      failed = 1;
    } else {
      const struct __weavec_rt_found found = __weavec_rt_find(at);
      failed = accessFails(&found, (uintptr_t)at, width, from);
    }
  }
  if (__builtin_expect(!failed, 1))
    return;
  if (site != NULL) {
    __weavec_rt_report(guardName(kind), site->file, site->line, site->column);
    return;
  }
  __weavec_rt_trapping();
  __builtin_trap();
}

int __weavec_rt_release_ok(const void *p) {
  WEAVEC_RT_FORWARD(releaseOk, p);
  struct __weavec_rt_found found;
  if (p == NULL)
    return 0;
  found = __weavec_rt_find(p);
  if (found.state == WeavecRtUntracked)
    return weavecRtIsStackOrGlobal(p);
  if (found.state == WeavecRtTrackedDead || found.base != (uintptr_t)p)
    return 1;
  /* Live and at its start: a heap block, unless it is a stack or global
   * object. */
  return weavecRtIsStackOrGlobal(p);
}

/*===-- This image's descriptors --------------------------------------------===*/

#if defined(__APPLE__)
extern const void *const weavecGlobalsBegin[] __asm(
    "section$start$__DATA$__weavec_glob");
extern const void *const weavecGlobalsEnd[] __asm(
    "section$end$__DATA$__weavec_glob");
#define WEAVEC_GLOBALS_SECTION "__DATA,__weavec_glob"
#else
extern const void *const __start_weavec_globals[]
    __attribute__((visibility("hidden")));
extern const void *const __stop_weavec_globals[]
    __attribute__((visibility("hidden")));
#define weavecGlobalsBegin __start_weavec_globals
#define weavecGlobalsEnd __stop_weavec_globals
#define WEAVEC_GLOBALS_SECTION "weavec_globals"
#endif

/* One empty descriptor, so that the section exists in every image. */
static const void *const noGlobal[2]
    __attribute__((used, section(WEAVEC_GLOBALS_SECTION))) = {0, 0};

__attribute__((constructor)) static void addImageGlobals(void) {
  __weavec_rt_globals_add(weavecGlobalsBegin, weavecGlobalsEnd);
}
