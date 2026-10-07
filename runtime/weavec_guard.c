/*===- weavec_guard.c - Guards, frames and globals -----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, sections 2 to 5. The guard pass checks the shadow inline and
|* calls the slow path when a byte it reads is not 0; the slow path decides
|* exactly, from the shadow alone, and names a failure by the shadow value
|* that caused it. Frames and globals poison their redzones; this file
|* clears what a non-local exit left behind and registers the globals.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <pthread.h>
#include <string.h>

/*===-- Deciding (section 2.3) ---------------------------------------------===*/

/* Whether a shadow value is a poison (not a granule's addressable count). */
static inline int isPoison(unsigned char value) {
  return value >= WeavecRtShadowGranule;
}

/* The name of a failure whose first unaddressable byte has shadow `value`;
 * for a partial granule, the poison after it says whose object it is. */
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
      return "buffer-overflow";
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
  case WeavecRtShadowStackScope:
    return "stack-use-after-scope";
  case WeavecRtShadowGlobal:
    return "global-buffer-overflow";
  case WeavecRtShadowNull:
    return "null-dereference";
  default:
    return "invalid-access";
  }
}

/* The first byte of [address, address + n) that is not addressable: 0 when
 * there is none (or the shadow cannot say: before it exists, outside the
 * window). `value` receives its shadow byte. */
static int firstUnaddressable(uintptr_t address, uint64_t n, uintptr_t *bad,
                              unsigned char *value) {
  uintptr_t end;
  uintptr_t granule;
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
    unsigned char v = *weavecRtShadowOf(granule);
    if (v == WeavecRtShadowAddressable) {
      /* Eight granules at a time while they are all addressable. */
      while (granule + 128 <= end && (granule & 127) == 0) {
        uint64_t eight;
        memcpy(&eight, weavecRtShadowOf(granule), sizeof eight);
        if (eight != 0)
          break;
        granule += 128;
      }
      if (granule >= end)
        break;
      v = *weavecRtShadowOf(granule);
      if (v == WeavecRtShadowAddressable) {
        granule += 16;
        continue;
      }
    }
    {
      const uintptr_t low = granule > address ? granule : address;
      const uintptr_t high = granule + 16 < end ? granule + 16 : end;
      if (!isPoison(v)) {
        /* The first v bytes are addressable. */
        if (high <= granule + v) {
          granule += 16;
          continue;
        }
        *bad = low > granule + v ? low : granule + v;
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
  /* A partial granule of the arena or a huge block is a heap object's. */
  if (!isPoison(value) && weavecRtHeapObjectAt(bad).found)
    value = WeavecRtShadowHeapTail;
  weavecRtFail(kindOf(value, bad & ~(uintptr_t)15), address, width, site);
}

WEAVEC_RT_SLOW void __weavec_rt_guard(uintptr_t address, uint64_t width,
                       const struct __weavec_rt_site *site) {
  uintptr_t bad;
  unsigned char value;
  weavecRtCount(WeavecRtStatSlowGuards);
  if (firstUnaddressable(address, width, &bad, &value))
    failAt(bad, value, address, width, site);
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
    if (eight != 0)
      return 0;
  }
  for (; i + 1 < granules; ++i)
    if (shadow[i] != 0)
      return 0;
  value = shadow[granules - 1];
  return value == WeavecRtShadowAddressable ||
         (value < WeavecRtShadowGranule && (last & 15) < value);
}

void __weavec_rt_range(uintptr_t address, uint64_t n,
                       const struct __weavec_rt_site *site) {
  uintptr_t bad;
  unsigned char value;
  if (weavecRtQuickOk(address, n) || allAddressable(address, n))
    return;
  weavecRtCount(WeavecRtStatRanges);
  if (firstUnaddressable(address, n, &bad, &value))
    failAt(bad, value, address, n, site);
}

int __weavec_rt_range_ok(uintptr_t address, uint64_t n) {
  uintptr_t bad;
  unsigned char value;
  if (weavecRtQuickOk(address, n) || allAddressable(address, n))
    return 1;
  weavecRtCount(WeavecRtStatRanges);
  return !firstUnaddressable(address, n, &bad, &value);
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
      firstUnaddressable((uintptr_t)s, scanned, &bad, &value))
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
    if ((shadow[0] | shadow[1] | ((start & 15) != 0 ? shadow[2] : 0)) == 0) {
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

/* The calling thread's stack top (its highest address), or 0. */
static uintptr_t stackTop(void) {
  static __thread uintptr_t top;
  if (top == 0) {
    pthread_t self = pthread_self();
#if defined(__APPLE__)
    top = (uintptr_t)pthread_get_stackaddr_np(self);
#elif defined(__linux__)
    pthread_attr_t attributes;
    void *low = NULL;
    size_t size = 0;
    if (pthread_getattr_np(self, &attributes) == 0) {
      if (pthread_attr_getstack(&attributes, &low, &size) == 0 && low != NULL)
        top = (uintptr_t)low + size;
      (void)pthread_attr_destroy(&attributes);
    }
#else
    (void)self;
#endif
  }
  return top;
}

void __weavec_rt_unpoison_stack(void) {
  const uintptr_t low = (uintptr_t)__builtin_frame_address(0);
  const uintptr_t top = stackTop();
  weavecRtCount(WeavecRtStatStackUnpoisons);
  /* A stack the thread runs on that is not its own (a coroutine's, an
   * alternate signal stack) is left alone: its extent is not known. */
  if (top > low && top - low <= ((uintptr_t)1 << 30))
    weavecRtShadowClear(low, top);
}

void __weavec_rt_alloca_poison(uintptr_t address, uint64_t size) {
  const uint64_t used = (size + 15) & ~(uint64_t)15;
  const uint64_t padded = ((size + 31) & ~(uint64_t)31) + 32;
  if ((address & 15) != 0)
    return;
  weavecRtShadowObject(address, size, WeavecRtShadowStackDynamic);
  weavecRtShadowSet(address + used, padded - used, WeavecRtShadowStackDynamic);
}

void __weavec_rt_alloca_unpoison(uintptr_t low, uintptr_t high) {
  if (high > low && high - low <= ((uintptr_t)1 << 30))
    weavecRtShadowClear(low, high);
}

/*===-- Globals (section 4) ------------------------------------------------===*/

void __weavec_rt_globals_register(const struct __weavec_rt_global *globals,
                                  uint64_t n) {
  uint64_t i;
  (void)weavecRtInitialise();
  for (i = 0; i < n; ++i) {
    const struct __weavec_rt_global *global = &globals[i];
    const uint64_t used = (global->size + 15) & ~(uint64_t)15;
    if ((global->address & 15) != 0 || global->padded < used)
      continue;
    weavecRtShadowObject(global->address, global->size, WeavecRtShadowGlobal);
    if (global->padded > used)
      weavecRtShadowSet(global->address + used, global->padded - used,
                        WeavecRtShadowGlobal);
  }
}

void __weavec_rt_globals_unregister(const struct __weavec_rt_global *globals,
                                    uint64_t n) {
  uint64_t i;
  for (i = 0; i < n; ++i)
    weavecRtShadowClear(globals[i].address,
                        globals[i].address + globals[i].padded);
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

WEAVEC_RT_API __attribute__((weak)) void
__ubsan_handle_out_of_bounds(void *data, uintptr_t index);
WEAVEC_RT_API __attribute__((weak, noreturn)) void
__ubsan_handle_out_of_bounds_abort(void *data, uintptr_t index);

void __ubsan_handle_out_of_bounds(void *data, uintptr_t index) {
  const struct weavecRtUbsanOutOfBounds *bounds =
      (const struct weavecRtUbsanOutOfBounds *)data;
  struct __weavec_rt_site site = {bounds->location.file, bounds->location.line,
                                  bounds->location.column, WeavecRtSiteReport};
  weavecRtFail("index-out-of-bounds", index, 0, &site);
}

void __ubsan_handle_out_of_bounds_abort(void *data, uintptr_t index) {
  const struct weavecRtUbsanOutOfBounds *bounds =
      (const struct weavecRtUbsanOutOfBounds *)data;
  struct __weavec_rt_site site = {bounds->location.file, bounds->location.line,
                                  bounds->location.column, 0};
  weavecRtFail("index-out-of-bounds", index, 0, &site);
  __builtin_trap();
}
