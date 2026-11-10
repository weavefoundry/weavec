/*===- weavec_guard.c - Guards, frames and globals -----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, sections 2 to 5, and RFC 0036, sections 4 and 6. The guard
|* pass checks the shadow inline and calls the slow path when a byte it
|* reads is not 0; the slow path decides exactly, from the shadow alone, and
|* names a failure by the shadow value that caused it. Frames and globals
|* poison their redzones; this file clears what a non-local exit left behind
|* and registers the globals, in the process's runtime.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <pthread.h>
#include <string.h>
#include <sys/mman.h>

/*===-- Deciding (section 2.3) ---------------------------------------------===*/

/* Whether a shadow value is a poison (not an addressable granule's). */
static inline int isPoison(unsigned char value) {
  return weavecRtShadowPoison(value);
}

/* The name of a failure whose first unaddressable byte has shadow `value`;
 * for a partial granule, the poison after it says whose object it is (one
 * with none after it is the program's, through ASan's interface). */
static const char *kindOf(unsigned char value, uintptr_t granule) {
  unsigned look;
  if (!isPoison(value)) {
    for (look = 1; look < 64; ++look) {
      const uintptr_t next = granule + ((uintptr_t)look << 4);
      if (!weavecRtInWindow(next))
        break;
      value = *weavecRtShadowOf(next);
      if (isPoison(value))
        break;
    }
    if (!isPoison(value))
      return "use-after-poison";
  }
  switch (value) {
  case WeavecRtShadowHeapTail:
    return "heap-buffer-overflow";
  case WeavecRtShadowHeapFreed:
    return "heap-use-after-free";
  case WeavecRtShadowStackLeft:
  case WeavecRtShadowStackMid:
  case WeavecRtShadowStackRight:
    return "stack-buffer-overflow";
  case WeavecRtShadowStackDynamic:
    return "dynamic-stack-buffer-overflow";
  case WeavecRtShadowReturned:
    return "stack-use-after-return";
  case WeavecRtShadowStackScope:
    return "stack-use-after-scope";
  case WeavecRtShadowUser:
    return "use-after-poison";
  case WeavecRtShadowGlobal:
    return "global-buffer-overflow";
  case WeavecRtShadowContainer:
    return "container-overflow";
  case WeavecRtShadowNull:
    return "null-dereference";
  default:
    return "invalid-access";
  }
}

/* RFC 0037 section 9: a granule a returned frame left (0xF5) below `here`
 * (the frame of the runtime's entry point, just below its caller's) on the
 * calling thread's own stack is a frame that returned; any other is stale
 * (a frame of code without WeaveC, or one with no tracked locals, holds the
 * bytes now, or the access runs on another stack): it is cleared, and the
 * access decided as if it were addressable. */
static unsigned char settle(uintptr_t granule, unsigned char v,
                            uintptr_t here) {
  if (v != WeavecRtShadowReturned)
    return v;
  if (granule + 16 <= here && granule >= weavecRtStackBottom() &&
      here < weavecRtStackTop())
    return v;
  *weavecRtShadowOf(granule) = WeavecRtShadowAddressable;
  return WeavecRtShadowAddressable;
}

/* The first byte of [address, address + n) that is not addressable: 0 when
 * there is none (or the shadow cannot say: before it exists, outside the
 * window). `value` receives its shadow byte. */
static int firstUnaddressable(uintptr_t address, uint64_t n, uintptr_t *bad,
                              unsigned char *value, uintptr_t here) {
  uintptr_t end;
  uintptr_t granule;
#if defined(__aarch64__)
  /* RFC 0037 section 4: the hardware ignores an address's top byte, so a
   * tagged address outside the window is decided as the one it reaches. */
  if (!weavecRtInWindow(address) &&
      weavecRtInWindow(address & ~((uintptr_t)0xFF << 56)))
    address &= ~((uintptr_t)0xFF << 56);
#endif
  if (n == 0 || !weavecRtShadowReady() || !weavecRtInWindow(address))
    return 0;
  if (__builtin_add_overflow(address, (uintptr_t)n, &end) ||
      !weavecRtInWindow(end - 1)) {
    /* A range that leaves the window: the part inside it is decided. */
    const unsigned bits = __weavec_rt_shadow.bits;
    end = bits >= 64 ? ~(uintptr_t)0 : (uintptr_t)1 << bits;
  }
  granule = address & ~(uintptr_t)15;
  while (granule < end) {
    unsigned char v = settle(granule, *weavecRtShadowOf(granule), here);
    if (weavecRtShadowFull(v)) {
      /* Eight granules at a time while they are all whole. */
      while (granule + 128 <= end && (granule & 127) == 0) {
        uint64_t eight;
        memcpy(&eight, weavecRtShadowOf(granule), sizeof eight);
        if ((eight & 0x8080808080808080ULL) != 0)
          break;
        granule += 128;
      }
      if (granule >= end)
        break;
      v = settle(granule, *weavecRtShadowOf(granule), here);
      if (weavecRtShadowFull(v)) {
        granule += 16;
        continue;
      }
    }
    {
      const uintptr_t low = granule > address ? granule : address;
      const uintptr_t high = granule + 16 < end ? granule + 16 : end;
      if (!isPoison(v)) {
        /* The first k bytes are addressable. */
        const unsigned k = weavecRtShadowBytes(v);
        if (high <= granule + k) {
          granule += 16;
          continue;
        }
        *bad = low > granule + k ? low : granule + k;
      } else {
        *bad = low;
      }
      *value = v;
      return 1;
    }
  }
  return 0;
}

static void failAt(uintptr_t bad, unsigned char value, uintptr_t address,
                   uint64_t width, const struct __weavec_rt_site *site) {
  /* A partial granule of the arena or a huge block is a heap object's: its
   * end, or its container's elements' (RFC 0039 section 5). */
  const struct weavecRtHeapObject object = weavecRtHeapObjectAt(bad);
  const uintptr_t next = (bad | 15) + 1;
  if (!isPoison(value) && object.found)
    value = next < object.base + object.size &&
                    *weavecRtShadowOf(next) == WeavecRtShadowContainer
                ? WeavecRtShadowContainer
                : WeavecRtShadowHeapTail;
  weavecRtFail(kindOf(value, bad & ~(uintptr_t)15), address, width, site);
}

__attribute__((noinline)) static void
guardSlow(uintptr_t address, uint64_t width,
          const struct __weavec_rt_site *site, uintptr_t here) {
  uintptr_t bad;
  unsigned char value;
  weavecRtCount(WeavecRtStatSlowGuards);
  if (firstUnaddressable(address, width, &bad, &value, here))
    failAt(bad, value, address, width, site);
}

WEAVEC_RT_SLOW void __weavec_rt_guard(uintptr_t address, uint64_t width,
                       const struct __weavec_rt_site *site) {
  guardSlow(address, width, site, (uintptr_t)__builtin_frame_address(0));
}

/*===-- Colors (RFC 0037 section 8.3) -------------------------------------===*/

/* Set once a non-local exit cleared the thread's stack's shadow (and the
 * colors of frames that live on); per thread (RFC 0039 section 3). */
static __thread int stackLostColors;

/* Whether `address` lies on the calling thread's stack, above `here`. */
static int onStack(uintptr_t address, uintptr_t here) {
  const uintptr_t top = weavecRtStackTop();
  return address >= here && address < top;
}

/* A color mismatch is decided exactly where it can be (the heap) and by
 * color where it cannot (frames, globals). A root outside any object (one
 * past it, before it, released) cannot be compared. */
/* The kind of the failure of an access to `address` computed from `root`,
 * whose shadow byte was `rootShadow`, or null when it passes. */
static const char *jumpKind(uintptr_t address, uintptr_t root,
                            unsigned char rootShadow, uintptr_t here) {
  unsigned char v;
  if (!weavecRtShadowReady() || !weavecRtInWindow(address))
    return NULL;
  v = *weavecRtShadowOf(address);
  if (isPoison(v) || isPoison(rootShadow))
    return NULL;
  if (v == rootShadow ||
      ((!weavecRtShadowFull(v) || !weavecRtShadowFull(rootShadow)) &&
       weavecRtShadowClass(v) == weavecRtShadowClass(rootShadow)))
    return NULL;
  /* A root in uncolored memory (untracked memory, a huge block, dead
   * stack, a frame a non-local exit cleared) has no object to hold the
   * access to; nor one whose shadow is uncolored now (RFC 0039 am. 16). */
  if (rootShadow == 0 || *weavecRtShadowOf(root) == 0)
    return NULL;
  /* A frame that lives on after a non-local exit cleared the stack's shadow
   * has lost its locals' colors. */
  if (v == 0 && stackLostColors && onStack(address, here))
    return NULL;
  {
    const struct weavecRtHeapObject target = weavecRtHeapObjectAt(address);
    const struct weavecRtHeapObject from = weavecRtHeapObjectAt(root);
    /* One heap object; or a root inside a live heap object other than at
     * its start, which the program biased or moved on purpose (a one-based
     * array, a compressor's window base): only an object's start is held to
     * its color on the heap (RFC 0037 amendment 12). */
    if (from.found && (target.base == from.base || from.base != root))
      return NULL;
    if (target.found)
      return "heap-buffer-overflow";
    if (onStack(address, here))
      return "stack-buffer-overflow";
  }
  return "global-buffer-overflow";
}

__attribute__((noinline)) static void
jumpSlow(uintptr_t address, uint64_t width, uintptr_t root,
         unsigned char rootShadow, const struct __weavec_rt_site *site,
         uintptr_t here) {
  const char *kind;
  if (width == 0)
    return;
  kind = jumpKind(address, root, rootShadow, here);
  if (kind != NULL)
    weavecRtFailJump(kind, address, width, root, site);
}

/* RFC 0037 section 8.5: whether a loop range starting at `address`, from
 * `root`, may run without its color checks. */
int __weavec_rt_color_ok(uintptr_t root, uintptr_t address) {
  if (!weavecRtShadowReady() || !weavecRtInWindow(root))
    return 1;
  return jumpKind(address, root, *weavecRtShadowOf(root),
                  (uintptr_t)__builtin_frame_address(0)) == NULL;
}

/* An inline check with a root: the guard first, then the color. */
__attribute__((noinline)) static void
guardJumpSlow(uintptr_t address, uint64_t width, uintptr_t root,
              unsigned char rootShadow, const struct __weavec_rt_site *site,
              uintptr_t here) {
  guardSlow(address, width, site, here);
  jumpSlow(address, width, root, rootShadow, site, here);
}

WEAVEC_RT_SLOW void __weavec_rt_guard_jump(uintptr_t address, uint64_t width,
                                           uintptr_t root, uint64_t rootShadow,
                                           const struct __weavec_rt_site *site) {
  guardJumpSlow(address, width, root, (unsigned char)rootShadow, site,
                (uintptr_t)__builtin_frame_address(0));
}

WEAVEC_RT_SLOW void __weavec_rt_jump(uintptr_t address, uint64_t width,
                                     uintptr_t root, uint64_t rootShadow,
                                     const struct __weavec_rt_site *site) {
  jumpSlow(address, width, root, (unsigned char)rootShadow, site,
           (uintptr_t)__builtin_frame_address(0));
}

void __weavec_rt_overlap(uintptr_t destination, uintptr_t source, uint64_t n,
                         const struct __weavec_rt_site *site) {
  (void)source;
  weavecRtFail("overlapping-copy", destination, n, site);
}

WEAVEC_RT_SLOW void __weavec_rt_null(uintptr_t address,
                                     const struct __weavec_rt_site *site) {
  weavecRtFail("null-dereference", address, 0, site);
}

/* Whether every byte of [address, address + n) is addressable, for a
 * range inside the window whose shadow is contiguous: every granule's
 * shadow byte but the last is 0, read eight at a time, and the last holds
 * the range's last byte. 0 also when that cannot be decided so. */
static int allAddressable(uintptr_t address, uint64_t n) {
  const uintptr_t last = address + n - 1;
  const unsigned char *shadow;
  uint64_t granules;
  uint64_t i = 0;
  unsigned char value;
  if (n == 0 || n > ((uint64_t)1 << 30) || last < address ||
      !weavecRtShadowReady() || !weavecRtInWindow(address) ||
      !weavecRtInWindow(last))
    return 0;
  shadow = weavecRtShadowOf(address);
  granules = (last >> 4) - (address >> 4) + 1;
  for (; i + 8 < granules; i += 8) {
    uint64_t eight;
    memcpy(&eight, shadow + i, sizeof eight);
    if ((eight & 0x8080808080808080ULL) != 0)
      return 0;
  }
  for (; i + 1 < granules; ++i)
    if (!weavecRtShadowFull(shadow[i]))
      return 0;
  value = shadow[granules - 1];
  return weavecRtShadowFull(value) ||
         (!isPoison(value) && (last & 15) < weavecRtShadowBytes(value));
}

void __weavec_rt_range(uintptr_t address, uint64_t n,
                       const struct __weavec_rt_site *site) {
  uintptr_t bad;
  unsigned char value;
  if (weavecRtQuickOk(address, n) || allAddressable(address, n))
    return;
  weavecRtCount(WeavecRtStatRanges);
  if (firstUnaddressable(address, n, &bad, &value,
                         (uintptr_t)__builtin_frame_address(0)))
    failAt(bad, value, address, n, site);
}

int __weavec_rt_range_ok(uintptr_t address, uint64_t n) {
  uintptr_t bad;
  unsigned char value;
  if (weavecRtQuickOk(address, n) || allAddressable(address, n))
    return 1;
  weavecRtCount(WeavecRtStatRanges);
  return !firstUnaddressable(address, n, &bad, &value,
                             (uintptr_t)__builtin_frame_address(0));
}

/* The C library's scan, then the shadow of what it scanned: a sixteenth of
 * the bytes. A scan that left the string's object read bytes the program
 * never sees: the guard fails before the call returns. */
static uint64_t stringLength(const char *s, uint64_t max,
                             const struct __weavec_rt_site *site) {
  const uint64_t length = strnlen(s, max);
  const uint64_t scanned = length < max ? length + 1 : max;
  uintptr_t bad;
  unsigned char value;
  if (!weavecRtQuickOk((uintptr_t)s, scanned) &&
      firstUnaddressable((uintptr_t)s, scanned, &bad, &value,
                         (uintptr_t)__builtin_frame_address(0)))
    weavecRtFail(value == WeavecRtShadowHeapFreed ? "heap-use-after-free"
                                                  : "unterminated-string",
                 (uintptr_t)s, scanned, site);
  return length;
}

/* Whether one of the eight bytes of `word` is zero. */
static inline int hasZeroByte(uint64_t word) {
  return ((word - 0x0101010101010101ULL) & ~word & 0x8080808080808080ULL) != 0;
}

uint64_t __weavec_rt_strlen(const char *s, uint64_t max,
                            const struct __weavec_rt_site *site) {
  const uintptr_t start = (uintptr_t)s;
  weavecRtCount(WeavecRtStatStrings);
  /* A null string is the call's to accept (system(NULL)) or an access
   * through null. */
  if (s == NULL) {
    if (site == NULL || (site->flags & WeavecRtSiteNullOk) == 0)
      weavecRtFail("null-dereference", 0, 1, site);
    return 0;
  }
  /* Most strings end within their first 32 bytes: when the granules those
   * lie in are addressable, they are scanned here, a word at a time. */
  /* (Inside one 4 KiB page: the shadow of untracked memory says
   * addressable, and the next page may not be mapped.) */
  if (max >= 32 && (start & 4095) <= 4096 - 32 && weavecRtShadowReady() &&
      weavecRtInWindow(start + 32)) {
    const unsigned char *shadow = weavecRtShadowOf(start);
    if (((shadow[0] | shadow[1] | ((start & 15) != 0 ? shadow[2] : 0)) &
         0x80) == 0) {
      uint64_t words[4];
      unsigned i;
      memcpy(words, s, sizeof words);
      for (i = 0; i < 4; ++i)
        if (hasZeroByte(words[i])) {
          const char *at = s + 8 * i;
          while (*at != 0)
            ++at;
          return (uint64_t)(at - s);
        }
    }
  }
  return stringLength(s, max, site);
}

/*===-- Frames (section 3) -------------------------------------------------===*/

/* The calling thread's own stack, [low, top), cached per thread. */
static __thread uintptr_t stackLow;
static __thread uintptr_t stackHigh;

static void readStack(void) {
  pthread_t self = pthread_self();
#if defined(__APPLE__)
  stackHigh = (uintptr_t)pthread_get_stackaddr_np(self);
  stackLow = stackHigh - pthread_get_stacksize_np(self);
#elif defined(__linux__)
  /* It frees under the thread's lock; freeing a libc block asks again. */
  static __thread int reading;
  pthread_attr_t attributes;
  void *low = NULL;
  size_t size = 0;
  if (reading)
    return;
  reading = 1;
  if (pthread_getattr_np(self, &attributes) == 0) {
    if (pthread_attr_getstack(&attributes, &low, &size) == 0 && low != NULL) {
      stackLow = (uintptr_t)low;
      stackHigh = (uintptr_t)low + size;
    }
    (void)pthread_attr_destroy(&attributes);
  }
  reading = 0;
#else
  (void)self;
#endif
}

uintptr_t weavecRtStackTop(void) {
  if (stackHigh == 0)
    readStack();
  return stackHigh;
}

uintptr_t weavecRtStackBottom(void) {
  if (stackHigh == 0)
    readStack();
  return stackLow;
}

/* Whether [address, end) overlaps the calling thread's own stack. Another
 * stack (a coroutine's, an alternate signal stack) has no known extent: a
 * frame's or an object's shadow on it is written up to 1 GiB, as
 * __weavec_rt_unpoison_stack's is. */
static int onOwnStack(uintptr_t address, uintptr_t end) {
  const uintptr_t top = weavecRtStackTop();
  return top != 0 && address < top && end > weavecRtStackBottom();
}

/* RFC 0037 section 4: a run of a frame's shadow too long for stores, for
 * the part of [address, address + bytes) that lies on the calling thread's
 * stack: a frame larger than the stack cannot be used without leaving it,
 * and its first access faults, as without WeaveC. */
void __weavec_rt_frame_shadow(uintptr_t address, uint64_t bytes,
                              uint64_t value) {
  uintptr_t end;
  if (__builtin_add_overflow(address, (uintptr_t)bytes, &end))
    end = ~(uintptr_t)0;
  if (onOwnStack(address, end)) {
    if (address < weavecRtStackBottom())
      address = weavecRtStackBottom();
    if (end > weavecRtStackTop())
      end = weavecRtStackTop();
  } else if (end - address > ((uintptr_t)1 << 30)) {
    return;
  }
  if (end <= address)
    return;
  if (value == WeavecRtShadowAddressable)
    weavecRtShadowClear(address, end);
  else
    weavecRtShadowSet(address, end - address, (unsigned char)value);
}

void __weavec_rt_unpoison_stack(void) {
  const uintptr_t low = (uintptr_t)__builtin_frame_address(0);
  const uintptr_t top = weavecRtStackTop();
  weavecRtCount(WeavecRtStatStackUnpoisons);
  stackLostColors = 1;
  /* A stack the thread runs on that is not its own (a coroutine's, an
   * alternate signal stack) is left alone: its extent is not known. */
  if (top > low && top - low <= ((uintptr_t)1 << 30))
    weavecRtShadowClear(low, top);
}

void __weavec_rt_alloca_poison(uintptr_t address, uint64_t size) {
  const uint64_t used = (size + 15) & ~(uint64_t)15;
  const uint64_t padded = ((size + 31) & ~(uint64_t)31) + 32;
  uintptr_t end;
  /* RFC 0037 section 4: an object that does not fit on the thread's stack
   * (a variable-length array of negative size wraps to one) is not written:
   * its first access leaves the stack and faults, as without WeaveC. */
  if ((address & 15) != 0 || padded < size ||
      __builtin_add_overflow(address, (uintptr_t)padded, &end) ||
      (onOwnStack(address, end)
           ? end > weavecRtStackTop() || address < weavecRtStackBottom()
           : padded > ((uint64_t)1 << 30)))
    return;
  weavecRtShadowObject(address, size, WeavecRtShadowStackDynamic, 0);
  weavecRtShadowSet(address + used, padded - used, WeavecRtShadowStackDynamic);
}

void __weavec_rt_alloca_unpoison(uintptr_t low, uintptr_t high) {
  if (high > low && high - low <= ((uintptr_t)1 << 30))
    weavecRtShadowClear(low, high);
}

/*===-- Globals (section 4) ------------------------------------------------===*/

static uint64_t globalColors;

/* RFC 0040 sections 5.3, 5.4: the units registered, for reports: their
 * globals and their frames' descriptors. */
typedef struct {
  const struct __weavec_rt_global *globals;
  uint64_t n;
  const struct __weavec_rt_frame *const *frames;
  uint64_t m;
} Unit;
static Unit *units;
static size_t unitCount;
static size_t unitRoom;
static WeavecRtLock unitLock;

static void addUnit(const Unit *unit) {
  weavecRtLock(&unitLock);
  if (unitCount == unitRoom) {
    const size_t room = unitRoom != 0 ? unitRoom * 2 : 256;
    Unit *grown = (Unit *)mmap(NULL, room * sizeof(Unit),
                               PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANON, -1, 0);
    if (grown == MAP_FAILED) {
      weavecRtUnlock(&unitLock);
      return;
    }
    weavecRtExcluded(grown, room * sizeof(Unit));
    if (unitCount != 0)
      memcpy(grown, units, unitCount * sizeof(Unit));
    /* (The old table stays mapped: a report may be reading it.) */
    units = grown;
    unitRoom = room;
  }
  units[unitCount] = *unit;
  __atomic_store_n(&unitCount, unitCount + 1, __ATOMIC_RELEASE);
  weavecRtUnlock(&unitLock);
}

void __weavec_rt_globals_register(const struct __weavec_rt_global *globals,
                                  uint64_t n,
                                  const struct __weavec_rt_frame *const *frames,
                                  uint64_t m) {
  const Unit unit = {globals, n, frames, m};
  uint64_t i;
  (void)weavecRtInitialize();
  addUnit(&unit);
  for (i = 0; i < n; ++i) {
    const struct __weavec_rt_global *global = &globals[i];
    const uint64_t used = (global->size + 15) & ~(uint64_t)15;
    if ((global->address & 15) != 0 || global->padded < used)
      continue;
    /* RFC 0037 section 7.2: the n-th global the process registers takes
     * the n-th color, so that a unit's globals differ. */
    weavecRtShadowObject(global->address, global->size, WeavecRtShadowGlobal,
                         weavecRtColor(__atomic_fetch_add(
                             &globalColors, 1, __ATOMIC_RELAXED)));
    if (global->padded > used)
      weavecRtShadowSet(global->address + used, global->padded - used,
                        WeavecRtShadowGlobal);
  }
}

void __weavec_rt_globals_unregister(const struct __weavec_rt_global *globals,
                                    uint64_t n,
                                    const struct __weavec_rt_frame *const *frames,
                                    uint64_t m) {
  size_t i;
  (void)frames;
  (void)m;
  weavecRtLock(&unitLock);
  for (i = 0; i < unitCount; ++i)
    if (units[i].globals == globals) {
      units[i] = units[unitCount - 1];
      --unitCount;
      break;
    }
  weavecRtUnlock(&unitLock);
  for (i = 0; i < n; ++i)
    weavecRtShadowClear(globals[i].address,
                        globals[i].address + globals[i].padded);
}

const struct __weavec_rt_global *weavecRtGlobalAt(uintptr_t address) {
  const size_t count = __atomic_load_n(&unitCount, __ATOMIC_ACQUIRE);
  size_t i;
  uint64_t j;
  for (i = 0; i < count; ++i)
    for (j = 0; j < units[i].n; ++j) {
      const struct __weavec_rt_global *global = &units[i].globals[j];
      if (address >= global->address &&
          address - global->address < global->padded)
        return global;
    }
  return NULL;
}

int weavecRtFrameKnown(const struct __weavec_rt_frame *frame) {
  const size_t count = __atomic_load_n(&unitCount, __ATOMIC_ACQUIRE);
  size_t i;
  uint64_t j;
  for (i = 0; i < count; ++i)
    for (j = 0; j < units[i].m; ++j)
      if (units[i].frames[j] == frame)
        return 1;
  return 0;
}

/*===-- Fortified sizes (RFC 0036 section 6) ------------------------------===*/

WEAVEC_RT_SLOW void __weavec_rt_limit_fail(uintptr_t address, uint64_t width,
                                           uint64_t limit,
                                           const struct __weavec_rt_site *site) {
  weavecRtFailObject("intra-object-overflow", address, width, address,
                     address + limit, site);
}

/*===-- Array bounds (section 5.4) -----------------------------------------===*/

/* Clang's `array-bounds` instrumentation calls these with its own site
 * record; UBSan's runtime, when linked, provides them instead. */
struct weavecRtUbsanLocation {
  const char *file;
  uint32_t line;
  uint32_t column;
};
struct weavecRtUbsanOutOfBounds {
  struct weavecRtUbsanLocation location;
  const void *arrayType;
  const void *indexType;
};

WEAVEC_RT_REPLACEABLE void
__ubsan_handle_out_of_bounds(void *data, uintptr_t index);
WEAVEC_RT_REPLACEABLE __attribute__((noreturn)) void
__ubsan_handle_out_of_bounds_abort(void *data, uintptr_t index);

void __ubsan_handle_out_of_bounds(void *data, uintptr_t index) {
  const struct weavecRtUbsanOutOfBounds *bounds =
      (const struct weavecRtUbsanOutOfBounds *)data;
  struct __weavec_rt_site site;
  weavecRtSiteSet(&site, bounds->location.file, bounds->location.line,
                  bounds->location.column, WeavecRtSiteReport);
  weavecRtFail("index-out-of-bounds", index, 0, &site);
}

void __ubsan_handle_out_of_bounds_abort(void *data, uintptr_t index) {
  const struct weavecRtUbsanOutOfBounds *bounds =
      (const struct weavecRtUbsanOutOfBounds *)data;
  struct __weavec_rt_site site;
  weavecRtSiteSet(&site, bounds->location.file, bounds->location.line,
                  bounds->location.column, 0);
  weavecRtFail("index-out-of-bounds", index, 0, &site);
  __builtin_trap();
}
