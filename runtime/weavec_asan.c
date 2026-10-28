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
|* (`use-after-poison`); what ASan does that WeaveC does not (leak
|* checking, fake stacks, stack traces) does nothing.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <string.h>

#define WEAVEC_ASAN_API WEAVEC_RT_API __attribute__((weak))

WEAVEC_ASAN_API void __asan_poison_memory_region(const volatile void *address,
                                                 size_t size);
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
  if (size == 0 || !weavecRtShadowReady() || !weavecRtInWindow(start))
    return;
  if ((end & ~(uintptr_t)15) > low)
    memset(weavecRtShadowOf(low), WeavecRtShadowAddressable,
           ((end & ~(uintptr_t)15) - low) >> 4);
  if ((end & 15) != 0) {
    unsigned char *last = weavecRtShadowOf(end);
    if (*last != WeavecRtShadowAddressable && *last < (end & 15))
      *last = (unsigned char)(end & 15);
    else if (*last >= WeavecRtShadowGranule)
      *last = (unsigned char)(end & 15);
  }
}

int __asan_address_is_poisoned(const volatile void *address) {
  return !__weavec_rt_range_ok((uintptr_t)address, 1);
}

void *__asan_region_is_poisoned(void *address, size_t size) {
  size_t at;
  if (size == 0 || __weavec_rt_range_ok((uintptr_t)address, size))
    return NULL;
  for (at = 0; at < size; ++at)
    if (!__weavec_rt_range_ok((uintptr_t)address + at, 1))
      return (char *)address + at;
  return NULL;
}

void __asan_describe_address(void *address) {
  const struct weavecRtHeapObject object =
      weavecRtHeapObjectAt((uintptr_t)address);
  (void)object;
}

void __asan_handle_no_return(void) { __weavec_rt_unpoison_stack(); }
void *__asan_get_current_fake_stack(void) { return NULL; }
void *__asan_addr_is_in_fake_stack(void *fake, void *address, void **begin,
                                   void **end) {
  (void)fake;
  (void)address;
  (void)begin;
  (void)end;
  return NULL;
}
void __sanitizer_print_stack_trace(void) {}
void __sanitizer_set_report_path(const char *path) { (void)path; }
void __sanitizer_annotate_contiguous_container(const void *begin,
                                               const void *end,
                                               const void *oldEnd,
                                               const void *newEnd) {
  (void)begin;
  (void)end;
  (void)oldEnd;
  (void)newEnd;
}
void __sanitizer_start_switch_fiber(void **save, const void *bottom,
                                    size_t size) {
  (void)save;
  (void)bottom;
  (void)size;
}
void __sanitizer_finish_switch_fiber(void *save, const void **bottom,
                                     size_t *size) {
  (void)save;
  (void)bottom;
  (void)size;
}
void __lsan_ignore_object(const void *p) { (void)p; }
void __lsan_disable(void) {}
void __lsan_enable(void) {}
void __lsan_register_root_region(const void *p, size_t size) {
  (void)p;
  (void)size;
}
void __lsan_unregister_root_region(const void *p, size_t size) {
  (void)p;
  (void)size;
}
void __lsan_do_leak_check(void) {}
int __lsan_do_recoverable_leak_check(void) { return 0; }
