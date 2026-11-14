/*===- weavec_asan.c - AddressSanitizer's interface ----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0036, section 5. weavec-cc compiles code as AddressSanitizer would
|* (`__SANITIZE_ADDRESS__`, `__has_feature(address_sanitizer)`), so code
|* written to be ASan-clean calls ASan's interface: an allocator that
|* poisons its free space, a coroutine library that announces its stack
|* switches, a test that asks whether an address is poisoned. These are
|* those functions over WeaveC's shadow: a poisoned granule is `0xF7`
|* (`use-after-poison`), storage past a container's elements `0xFC` (RFC
|* 0039 section 5); what WeaveC does not do (leak checks) does nothing.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <stdlib.h>
#include <string.h>

#define WEAVEC_ASAN_API WEAVEC_RT_REPLACEABLE

WEAVEC_ASAN_API void __asan_poison_memory_region(const volatile void *address,
                                                 size_t size);
WEAVEC_ASAN_API void __asan_poison_cxx_array_cookie(uintptr_t cookie);
WEAVEC_ASAN_API uintptr_t __asan_load_cxx_array_cookie(uintptr_t *cookie);
WEAVEC_ASAN_API void
__asan_unpoison_memory_region(const volatile void *address, size_t size);
WEAVEC_ASAN_API int __asan_address_is_poisoned(const volatile void *address);
WEAVEC_ASAN_API void *__asan_region_is_poisoned(void *address, size_t size);
WEAVEC_ASAN_API void __asan_describe_address(void *address);
WEAVEC_ASAN_API void __asan_handle_no_return(void);
WEAVEC_ASAN_API void *__asan_get_current_fake_stack(void);
WEAVEC_ASAN_API void *__asan_addr_is_in_fake_stack(void *fake, void *address,
                                                   void **begin, void **end);
WEAVEC_ASAN_API void __sanitizer_print_stack_trace(void);
WEAVEC_ASAN_API void __sanitizer_set_report_path(const char *path);
WEAVEC_ASAN_API void
__sanitizer_annotate_contiguous_container(const void *begin, const void *end,
                                          const void *oldEnd,
                                          const void *newEnd);
WEAVEC_ASAN_API void __sanitizer_start_switch_fiber(void **save,
                                                    const void *bottom,
                                                    size_t size);
WEAVEC_ASAN_API void __sanitizer_finish_switch_fiber(void *save,
                                                     const void **bottom,
                                                     size_t *size);
WEAVEC_ASAN_API void __lsan_ignore_object(const void *p);
WEAVEC_ASAN_API void __lsan_disable(void);
WEAVEC_ASAN_API void __lsan_enable(void);
WEAVEC_ASAN_API void __lsan_register_root_region(const void *p, size_t size);
WEAVEC_ASAN_API void __lsan_unregister_root_region(const void *p, size_t size);
WEAVEC_ASAN_API void __lsan_do_leak_check(void);
WEAVEC_ASAN_API int __lsan_do_recoverable_leak_check(void);

/* ASan poisons only whole granules inside the region (its granule is 8
 * bytes, WeaveC's 16), and leaves a partial last granule as it was. */
void __asan_poison_memory_region(const volatile void *address, size_t size) {
  const uintptr_t low = ((uintptr_t)address + 15) & ~(uintptr_t)15;
  const uintptr_t high = ((uintptr_t)address + size) & ~(uintptr_t)15;
  if (high > low)
    weavecRtShadowSet(low, high - low, WeavecRtShadowUser);
}

/* Unpoisons the region, its partial last granule as the first bytes of
 * that granule. */
void __asan_unpoison_memory_region(const volatile void *address, size_t size) {
  const uintptr_t start = (uintptr_t)address;
  const uintptr_t low = start & ~(uintptr_t)15;
  const uintptr_t end = start + size;
  /* RFC 0037 section 7.2: in the color of the heap slot that holds it. */
  const unsigned char color = weavecRtColorOfHeap(start);
  if (size == 0 || !weavecRtShadowReady() || !weavecRtInWindow(start))
    return;
  if ((end & ~(uintptr_t)15) > low)
    memset(weavecRtShadowOf(low), color, ((end & ~(uintptr_t)15) - low) >> 4);
  if ((end & 15) != 0) {
    unsigned char *last = weavecRtShadowOf(end);
    if (weavecRtShadowPoison(*last) ||
        (!weavecRtShadowFull(*last) && weavecRtShadowBytes(*last) < (end & 15)))
      *last = weavecRtShadowPartialOf(color, (unsigned)(end & 15));
  }
}

int __asan_address_is_poisoned(const volatile void *address) {
  return !__weavec_rt_range_ok((uintptr_t)address, 1);
}

void *__asan_region_is_poisoned(void *address, size_t size) {
  if (size == 0 || __weavec_rt_range_ok((uintptr_t)address, size))
    return NULL;
  for (size_t at = 0; at < size; ++at)
    if (!__weavec_rt_range_ok((uintptr_t)address + at, 1))
      return (char *)address + at;
  return NULL;
}

void __asan_describe_address(void *address) { (void)address; }

void __asan_handle_no_return(void) { __weavec_rt_unpoison_stack(); }
void *__asan_get_current_fake_stack(void) { return NULL; }
void *__asan_addr_is_in_fake_stack(void *fake, void *address, void **begin,
                                   void **end) {
  return (void)fake, (void)address, (void)begin, (void)end, NULL;
}
void __sanitizer_print_stack_trace(void) {}
void __sanitizer_set_report_path(const char *path) { (void)path; }
/* new[]'s count (Clang's Itanium ABI): unpoisoned; releases check sizes. */
void __asan_poison_cxx_array_cookie(uintptr_t cookie) { (void)cookie; }
uintptr_t __asan_load_cxx_array_cookie(uintptr_t *cookie) { return *cookie; }

/*===-- Containers (RFC 0039 section 5) ------------------------------------===*/

/* WEAVEC_RT_CONTAINERS=0 turns the annotations off (assumption A9); so does
 * a fast safe C++ unit (RFC 0045 section 9), whose copies of the containers
 * make none, unless WEAVEC_RT_CONTAINERS=1 asks for them. */
static int containersOff;
static int containersSet;
int weavecRtFastProgram;

void __weavec_rt_fast_program(void) {
  weavecRtFastProgram = 1;
  if (!containersSet)
    containersOff = 1;
  __weavec_rt_typed_program();
}
__attribute__((constructor)) static void readContainers(void) {
  const char *value = getenv("WEAVEC_RT_CONTAINERS");
  containersSet = value != NULL && (*value == '0' || *value == '1');
  containersOff = (value != NULL && strcmp(value, "0") == 0) ||
                  (!containersSet && weavecRtFastProgram);
}

/* Granule `g` of storage [begin, end) with elements [live, mid): the
 * container's outside them, else its first bytes up to `mid` (a prefix is
 * all the shadow encodes, so a front that splits a granule leaves it). */
static unsigned char containerGranule(uintptr_t g, uintptr_t begin,
                                      uintptr_t live, uintptr_t mid,
                                      unsigned char color) {
  if (g >= begin && (g + 16 <= live || g >= mid))
    return WeavecRtShadowContainer;
  return g + 16 <= mid ? color : weavecRtShadowPartialOf(color, mid - g);
}

/* Rewrites the granules of [from, to) in the heap block's color (outside the
 * heap, nothing); a last granule shared with bytes after the storage stays,
 * unless the block ends there too. */
static void annotate(uintptr_t begin, uintptr_t end, uintptr_t live,
                     uintptr_t mid, uintptr_t from, uintptr_t to) {
  const unsigned char color = weavecRtColorOfHeap(begin);
  uintptr_t g;
  if (containersOff || begin >= end || !weavecRtShadowReady() ||
      !weavecRtInWindow(begin) || !weavecRtInWindow(end - 1))
    return;
  from = (from < begin ? begin : from) & ~(uintptr_t)15;
  to = to > end ? end : to;
  if (color == 0 || (to > (end & ~(uintptr_t)15) && (end & 15) != 0)) {
    const struct weavecRtHeapObject object = weavecRtHeapObjectAt(begin);
    if (!object.found || !object.live)
      return;
    if ((end & 15) != 0 && end != object.base + object.size &&
        to > (end & ~(uintptr_t)15))
      to = end & ~(uintptr_t)15;
  }
  for (g = from; g < to; g += 16)
    *weavecRtShadowOf(g) = containerGranule(g, begin, live, mid, color);
}

void __sanitizer_annotate_contiguous_container(const void *begin,
                                               const void *end,
                                               const void *oldEnd,
                                               const void *newEnd) {
  const uintptr_t was = (uintptr_t)oldEnd;
  const uintptr_t now = (uintptr_t)newEnd;
  annotate((uintptr_t)begin, (uintptr_t)end, (uintptr_t)begin, now,
           was < now ? was : now, was < now ? now : was);
}

WEAVEC_ASAN_API void __sanitizer_annotate_double_ended_contiguous_container(
    const void *begin, const void *end, const void *oldBegin,
    const void *oldEnd, const void *newBegin, const void *newEnd) {
  /* The granules between the old and the new front, and the old and the
   * new back. */
  const uintptr_t live = (uintptr_t)newBegin, mid = (uintptr_t)newEnd;
  const uintptr_t front = (uintptr_t)oldBegin, back = (uintptr_t)oldEnd;
  annotate((uintptr_t)begin, (uintptr_t)end, live, mid,
           front < live ? front : live, (front < live ? live : front) + 16);
  annotate((uintptr_t)begin, (uintptr_t)end, live, mid,
           back < mid ? back : mid, (back < mid ? mid : back) + 16);
}

/* The first byte whose shadow differs from what [live, mid) make it. */
static const void *findBad(uintptr_t begin, uintptr_t live, uintptr_t mid,
                           uintptr_t end) {
  uintptr_t g;
  if (containersOff || begin >= end || !weavecRtShadowReady())
    return NULL;
  for (g = begin & ~(uintptr_t)15; g < end; g += 16) {
    const unsigned char want = containerGranule(g, begin, live, mid, 0);
    const unsigned char have = *weavecRtShadowOf(g);
    const uintptr_t at = g < begin ? begin : g;
    if (want == WeavecRtShadowContainer
            ? have != WeavecRtShadowContainer
            : weavecRtShadowPoison(have) ||
                  (!weavecRtShadowFull(want) &&
                   weavecRtShadowBytes(have) != weavecRtShadowBytes(want)))
      return (const void *)at;
  }
  return NULL;
}

WEAVEC_ASAN_API const void *__sanitizer_contiguous_container_find_bad_address(
    const void *begin, const void *mid, const void *end) {
  return findBad((uintptr_t)begin, (uintptr_t)begin, (uintptr_t)mid,
                 (uintptr_t)end);
}
WEAVEC_ASAN_API const void *
__sanitizer_double_ended_contiguous_container_find_bad_address(
    const void *begin, const void *live, const void *mid, const void *end) {
  return findBad((uintptr_t)begin, (uintptr_t)live, (uintptr_t)mid,
                 (uintptr_t)end);
}
WEAVEC_ASAN_API int __sanitizer_verify_contiguous_container(const void *begin,
                                                            const void *mid,
                                                            const void *end) {
  return findBad((uintptr_t)begin, (uintptr_t)begin, (uintptr_t)mid,
                 (uintptr_t)end) == NULL;
}
WEAVEC_ASAN_API int __sanitizer_verify_double_ended_contiguous_container(
    const void *begin, const void *live, const void *mid, const void *end) {
  return findBad((uintptr_t)begin, (uintptr_t)live, (uintptr_t)mid,
                 (uintptr_t)end) == NULL;
}

void __sanitizer_start_switch_fiber(void **save, const void *bottom,
                                    size_t size) {
  (void)save, (void)bottom, (void)size;
}
void __sanitizer_finish_switch_fiber(void *save, const void **bottom,
                                     size_t *size) {
  (void)save, (void)bottom, (void)size;
}
void __lsan_ignore_object(const void *p) { (void)p; }
void __lsan_disable(void) {}
void __lsan_enable(void) {}
void __lsan_register_root_region(const void *p, size_t size) {
  (void)p, (void)size;
}
void __lsan_unregister_root_region(const void *p, size_t size) {
  (void)p, (void)size;
}
void __lsan_do_leak_check(void) {}
int __lsan_do_recoverable_leak_check(void) { return 0; }
