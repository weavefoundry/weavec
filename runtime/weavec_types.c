/*===- weavec_types.c - Which type is at an address -----------------------===*
 *
 * Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
 * See LICENSE for license information.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 *
 *===----------------------------------------------------------------------===*
 *
 * RFC 0054 section 1: the runtime answers one question for a safe
 * function's checked conversions (a `void *` to a struct, a base to the
 * object that begins with it): is an object of type T at this address? It
 * keeps a table of the types the program's units describe (a layout each),
 * the type of each heap block (the allocator's slot records), of each
 * registered global, and of each local of a safe function whose address
 * leaves it (the frame records below). Memory with no record of its type is
 * believed, and counted (D5).
 *
 *===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>

/*===-- The table of types ------------------------------------------------===*/

/* A power of two; a program that describes more types than this records
 * none of the rest (their blocks are believed). */
enum { TypeSlots = 1u << 16 };

static const struct __weavec_rt_layout *types[TypeSlots];
static uint32_t typeCount;
static WeavecRtLock typeLock;
/* Index + 1 of each type, by its identity's hash. */
static uint32_t typeHash[TypeSlots];

static inline uint32_t hashOf(uint64_t identity) {
  uint64_t h = identity;
  h ^= h >> 33;
  h *= 0xFF51AFD7ED558CCDULL;
  h ^= h >> 33;
  return (uint32_t)h & (TypeSlots - 1);
}

uint32_t weavecRtTypeIndex(const struct __weavec_rt_layout *layout) {
  uint32_t at;
  uint32_t found = 0;
  if (layout == NULL)
    return 0;
  /* Without the lock first: an entry is written before its index. */
  for (at = hashOf(layout->identity);; at = (at + 1) & (TypeSlots - 1)) {
    const uint32_t index = __atomic_load_n(&typeHash[at], __ATOMIC_ACQUIRE);
    if (index == 0)
      break;
    if (types[index - 1]->identity == layout->identity)
      return index;
  }
  weavecRtLock(&typeLock);
  for (at = hashOf(layout->identity);; at = (at + 1) & (TypeSlots - 1)) {
    const uint32_t index = typeHash[at];
    if (index == 0) {
      /* (One slot of the hash stays empty, so that a search ends.) */
      if (typeCount + 1 < TypeSlots) {
        types[typeCount] = layout;
        found = ++typeCount;
        __atomic_store_n(&typeHash[at], found, __ATOMIC_RELEASE);
      }
      break;
    }
    if (types[index - 1]->identity == layout->identity) {
      found = index;
      break;
    }
  }
  weavecRtUnlock(&typeLock);
  return found;
}

const struct __weavec_rt_layout *weavecRtTypeLayout(uint32_t type) {
  return type != 0 && type <= __atomic_load_n(&typeCount, __ATOMIC_ACQUIRE)
             ? types[type - 1]
             : NULL;
}

/*===-- Frame records -----------------------------------------------------===*/

/* RFC 0054 section 1.2: the locals of a safe function whose address leaves
 * it, on a list each thread keeps, pushed where a local's scope begins and
 * popped where it ends. An entry below the stack pointer belongs to a frame
 * that is gone (a non-local exit skipped its pop) and is dropped where it
 * is met. A thread with more than FrameRecords recorded locals records no
 * more of them, which are then believed (as are all of a thread's when its
 * list cannot be mapped). */
enum { FrameRecords = 1024 };

struct FrameRecord {
  uintptr_t address;
  uint64_t size;
  const struct __weavec_rt_layout *layout;
};

/* The list is mapped on a thread's first push and unmapped when the thread
 * ends: the static TLS block keeps only a pointer and a depth, since a
 * runtime a program that weavec-cc did not link loads with dlopen must fit
 * glibc's small surplus of static TLS (the allocator's cache copy is
 * initial-exec), which a list of FrameRecords entries does not. */
static __thread struct FrameRecord *frameRecords;
static __thread uint32_t frameDepth;
static pthread_key_t frameKey;
static pthread_once_t frameKeyOnce = PTHREAD_ONCE_INIT;
static int frameKeyReady;

static void dropFrameRecords(void *list) {
  frameRecords = NULL;
  frameDepth = 0;
  (void)munmap(list, FrameRecords * sizeof(struct FrameRecord));
}

static void makeFrameKey(void) {
  frameKeyReady = pthread_key_create(&frameKey, dropFrameRecords) == 0;
}

/* The calling thread's list, mapped if it has none; null when it cannot
 * be. */
static struct FrameRecord *ownFrameRecords(void) {
  void *list;
  if (frameRecords != NULL)
    return frameRecords;
  (void)pthread_once(&frameKeyOnce, makeFrameKey);
  if (!frameKeyReady)
    return NULL;
  list = mmap(NULL, FrameRecords * sizeof(struct FrameRecord),
              PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (list == MAP_FAILED)
    return NULL;
  if (pthread_setspecific(frameKey, list) != 0) {
    (void)munmap(list, FrameRecords * sizeof(struct FrameRecord));
    return NULL;
  }
  frameRecords = (struct FrameRecord *)list;
  return frameRecords;
}

uint32_t __weavec_rt_frame_push(const void *address,
                                const struct __weavec_rt_layout *layout,
                                uint64_t size) {
  const uint32_t depth = frameDepth;
  const uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  struct FrameRecord *const list = ownFrameRecords();
  uint32_t top = depth;
  if (list == NULL)
    return depth;
  /* The entries of frames that are gone. */
  while (top != 0 && list[top - 1].address < sp)
    --top;
  if (top < FrameRecords) {
    list[top].address = (uintptr_t)address;
    list[top].size = size;
    list[top].layout = layout;
    frameDepth = top + 1;
  } else {
    frameDepth = top;
  }
  return top;
}

void __weavec_rt_frame_pop(uint32_t depth) {
  if (depth < frameDepth)
    frameDepth = depth;
}

/* The recorded local of this thread that holds `address`, or null. */
static const struct FrameRecord *recordAt(uintptr_t address) {
  const uintptr_t sp = (uintptr_t)__builtin_frame_address(0);
  uint32_t i = frameDepth;
  while (i != 0) {
    const struct FrameRecord *record = &frameRecords[--i];
    if (record->address < sp)
      continue;
    if (address >= record->address && address - record->address < record->size)
      return record;
  }
  return NULL;
}

/*===-- The question ------------------------------------------------------===*/

enum Answer { No, Yes, Believed };

/* Whether an object of `target` is `offset` bytes into an object of
 * `object`, which holds `extent` bytes from that object's start (an array
 * of it, when larger). */
/* Whether a pointer of `object`'s own bytes overlaps [offset, end). */
static int coversPointer(const struct __weavec_rt_layout *object,
                         uint64_t offset, uint64_t end) {
  uint32_t i;
  for (i = 0; i < object->count; ++i) {
    const struct __weavec_rt_part *part = &object->parts[i];
    uint32_t e;
    if ((part->flags & WeavecRtPartPointer) == 0)
      continue;
    for (e = 0; e < (part->stride == 0 ? 1 : part->elements); ++e) {
      const uint64_t at = part->offset + (uint64_t)e * part->stride;
      if (at < end && at + 8 > offset)
        return 1;
    }
  }
  return 0;
}

static enum Answer objectHolds(const struct __weavec_rt_layout *object,
                               uint64_t offset, uint64_t extent,
                               const struct __weavec_rt_layout *target) {
  uint32_t i;
  if (target->size != 0 && offset + target->size > extent)
    return No;
  /* A block holds an array of its type, when its size is a multiple of the
   * type's; the bytes after a first object of another size (data a program
   * keeps after a header, `s + 1`) have no record. */
  if (object->size != 0 && offset >= object->size &&
      extent % object->size != 0)
    return Believed;
  if (object->size != 0)
    offset %= object->size;
  if (offset == 0 && object->identity == target->identity)
    return Yes;
  /* Section 2.1 (d): a plain scalar views bytes where no pointer is. */
  if ((target->flags & WeavecRtLayoutScalar) != 0 &&
      (target->flags & WeavecRtLayoutPlain) != 0 &&
      ((object->flags & WeavecRtLayoutPlain) != 0 ||
       !coversPointer(object, offset, offset + target->size)))
    return Yes;
  for (i = 0; i < object->count; ++i) {
    const struct __weavec_rt_part *part = &object->parts[i];
    uint64_t at;
    if (part->identity != target->identity || part->identity == 0 ||
        (part->flags & (WeavecRtPartUnion | WeavecRtPartPointer)) != 0 ||
        offset < part->offset)
      continue;
    at = offset - part->offset;
    if (part->stride == 0 ? at == 0
                          : at % part->stride == 0 &&
                                at / part->stride < part->elements)
      return Yes;
  }
  return No;
}

/* The answer for `address`, and the layout of what holds it, if any. */
static enum Answer answer(uintptr_t address,
                          const struct __weavec_rt_layout *target) {
  struct weavecRtHeapObject block = weavecRtHeapObjectAt(address);
  if (block.found) {
    const struct __weavec_rt_layout *object = weavecRtTypeLayout(block.type);
    const uint64_t offset = address - block.base;
    if (object == NULL) {
      /* D5: a block with no type takes the type of its first checked
       * conversion, at its start, when the type fits; elsewhere in it the
       * conversion is believed. Plain types only view bytes. */
      if (target->size != 0 && offset + target->size > block.size)
        return No;
      if (offset == 0) {
        const uint32_t index = weavecRtTypeIndex(target);
        if (index != 0)
          weavecRtSetBlockType(block.base, index);
        return Yes;
      }
      return Believed;
    }
    return objectHolds(object, offset, block.size, target);
  }
  {
    const struct __weavec_rt_global *global = weavecRtGlobalAt(address);
    if (global != NULL) {
      const struct __weavec_rt_layout *object =
          global->layout != 0
              ? (const struct __weavec_rt_layout *)((const char *)&global
                                                        ->layout +
                                                    global->layout)
              : NULL;
      if (object == NULL)
        return Believed;
      return objectHolds(object, address - global->address, global->size,
                         target);
    }
  }
  {
    uintptr_t low = 0;
    uintptr_t high = 0;
    weavecRtOwnStack(&low, &high);
    if (address >= low && address < high) {
      const struct FrameRecord *record = recordAt(address);
      if (record != NULL)
        return objectHolds(record->layout, address - record->address,
                           record->size, target);
    }
  }
  return Believed;
}

/* The report: `bad-cast`, the address and the type, then the paragraph of
 * the object that holds the address, which names its type. */
static __attribute__((noinline)) void
badCast(const void *address, const struct __weavec_rt_layout *target,
        const struct __weavec_rt_site *site) {
  char message[320];
  (void)snprintf(message, sizeof message, "0x%lx is not a '%s'",
                 (unsigned long)(uintptr_t)address, weavecRtLayoutName(target));
  weavecRtFailTyped("bad-cast", message, (uintptr_t)address, site);
}

void __weavec_rt_cast(const void *address,
                      const struct __weavec_rt_layout *target,
                      const struct __weavec_rt_site *site) {
  enum Answer result;
  if (address == NULL || target == NULL)
    return;
  weavecRtCount(WeavecRtStatCasts);
  result = answer((uintptr_t)address, target);
  if (result == Believed)
    weavecRtCount(WeavecRtStatBelieved);
  else if (result == No)
    badCast(address, target, site);
}

void __weavec_rt_cast_fail(const void *address,
                           const struct __weavec_rt_layout *target,
                           const struct __weavec_rt_site *site) {
  badCast(address, target, site);
}

/*===-- Tagged unions (RFC 0054 section 3) --------------------------------===*/

static __attribute__((noinline)) void
badMember(uint64_t tag, const struct __weavec_rt_member *member,
          const struct __weavec_rt_site *site) {
  char message[320];
  const char *name =
      member->member != 0 ? (const char *)&member->member + member->member : "?";
  const char *field =
      member->tag != 0 ? (const char *)&member->tag + member->tag : "?";
  (void)snprintf(message, sizeof message, "'%s' %s while '%s' is %llu", name,
                 member->write != 0 ? "written" : "read", field,
                 (unsigned long long)tag);
  weavecRtFailTyped("union-member", message, 0, site);
}

void __weavec_rt_union_fail(uint64_t tag,
                            const struct __weavec_rt_member *member,
                            const struct __weavec_rt_site *site) {
  badMember(tag, member, site);
}
