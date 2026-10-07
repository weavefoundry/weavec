/*===- weavec_malloc.c - The image's allocation functions ---------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0032, section 2.5. The standard allocation functions, defined over
|* the arena for the image this archive (libweavec_alloc.a) is linked into.
|* Every call to `malloc` in the image, from WeaveC's units or any other
|* object, binds here. On ELF an executable's definitions also serve every
|* shared library; on Darwin other images keep the system allocator and
|* reach the arena through its malloc zone.
|*
|* The archive is separate from libweavec_rt.a so that a program that
|* defines the allocator itself links without it (section 2.6). The driver
|* reads what "the allocator" is from this archive: its strong definitions.
|* The rest are weak, so that a program's own `reallocarray` or
|* `posix_memalign` (a portability shim, usually over `malloc`) replaces
|* ours without a duplicate definition and without losing the arena.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

#define WEAVEC_RT_WEAK WEAVEC_RT_API __attribute__((weak))

/* The functions beyond the standard ones are defined only where the C
 * library exports them too: a definition the platform lacks would make a
 * configure probe find a function the reference compiler does not
 * (autoconf's link test of `reallocarray` on Darwin, whose libSystem has
 * only a variant symbol). On Darwin the C library's own versions reach the
 * arena through its malloc zone anyway. */
#if defined(__APPLE__)
#define WEAVEC_RT_REALLOCARRAY 0
#define WEAVEC_RT_FREE_SIZED 0
#elif defined(__GLIBC__)
#define WEAVEC_RT_REALLOCARRAY __GLIBC_PREREQ(2, 26)
#define WEAVEC_RT_FREE_SIZED __GLIBC_PREREQ(2, 41)
#else
#define WEAVEC_RT_REALLOCARRAY 1
#define WEAVEC_RT_FREE_SIZED 0
#endif

WEAVEC_RT_API void *malloc(size_t size);
WEAVEC_RT_API void *calloc(size_t count, size_t size);
WEAVEC_RT_API void *realloc(void *p, size_t size);
WEAVEC_RT_API void free(void *p);
#if WEAVEC_RT_REALLOCARRAY
WEAVEC_RT_WEAK void *reallocarray(void *p, size_t count, size_t size);
#endif
WEAVEC_RT_WEAK void *aligned_alloc(size_t alignment, size_t size);
WEAVEC_RT_WEAK int posix_memalign(void **slot, size_t alignment, size_t size);
WEAVEC_RT_WEAK void *valloc(size_t size);
#if WEAVEC_RT_FREE_SIZED
WEAVEC_RT_WEAK void free_sized(void *p, size_t size);
WEAVEC_RT_WEAK void free_aligned_sized(void *p, size_t alignment, size_t size);
#endif

void *malloc(size_t size) { return __weavec_rt_alloc(size, 0); }

void *calloc(size_t count, size_t size) {
  size_t bytes;
  if (__builtin_mul_overflow(count, size, &bytes)) {
    errno = ENOMEM;
    return NULL;
  }
  /* Every block is zero-filled. */
  return __weavec_rt_alloc(bytes, 0);
}

void *realloc(void *p, size_t size) { return __weavec_rt_realloc(p, size); }

#if WEAVEC_RT_REALLOCARRAY
void *reallocarray(void *p, size_t count, size_t size) {
  size_t bytes;
  if (__builtin_mul_overflow(count, size, &bytes)) {
    errno = ENOMEM;
    return NULL;
  }
  return __weavec_rt_realloc(p, bytes);
}
#endif

void free(void *p) { __weavec_rt_free(p); }

#if WEAVEC_RT_FREE_SIZED
void free_sized(void *p, size_t size) {
  (void)size;
  __weavec_rt_free(p);
}

void free_aligned_sized(void *p, size_t alignment, size_t size) {
  (void)alignment;
  (void)size;
  __weavec_rt_free(p);
}
#endif

static int isPowerOfTwo(size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

void *aligned_alloc(size_t alignment, size_t size) {
  if (!isPowerOfTwo(alignment)) {
    errno = EINVAL;
    return NULL;
  }
  return __weavec_rt_alloc(size, alignment);
}

int posix_memalign(void **slot, size_t alignment, size_t size) {
  void *block;
  const int saved = errno;
  if (!isPowerOfTwo(alignment) || alignment % sizeof(void *) != 0)
    return EINVAL;
  block = __weavec_rt_alloc(size, alignment);
  if (block == NULL) {
    errno = saved;
    return ENOMEM;
  }
  *slot = block;
  return 0;
}

static size_t pageSize(void) {
  const long page = sysconf(_SC_PAGESIZE);
  return page > 0 ? (size_t)page : 4096;
}

void *valloc(size_t size) { return __weavec_rt_alloc(size, pageSize()); }

#if defined(__APPLE__)

WEAVEC_RT_WEAK size_t malloc_size(const void *p);
WEAVEC_RT_WEAK size_t malloc_good_size(size_t size);
WEAVEC_RT_WEAK void *reallocf(void *p, size_t size);

size_t malloc_size(const void *p) { return __weavec_rt_size(p); }

size_t malloc_good_size(size_t size) { return size; }

void *reallocf(void *p, size_t size) {
  void *moved = __weavec_rt_realloc(p, size);
  if (moved == NULL && p != NULL && size != 0)
    __weavec_rt_free(p);
  return moved;
}

#else

WEAVEC_RT_WEAK void *memalign(size_t alignment, size_t size);
WEAVEC_RT_WEAK void *pvalloc(size_t size);
WEAVEC_RT_WEAK size_t malloc_usable_size(void *p);

void *memalign(size_t alignment, size_t size) {
  return aligned_alloc(alignment, size);
}

void *pvalloc(size_t size) {
  const size_t page = pageSize();
  size_t bytes;
  if (__builtin_add_overflow(size, page - 1, &bytes)) {
    errno = ENOMEM;
    return NULL;
  }
  return __weavec_rt_alloc(bytes / page * page, page);
}

size_t malloc_usable_size(void *p) { return __weavec_rt_size(p); }

/* Tells the runtime that the `malloc_usable_size` first in lookup order may
 * be the one above, which would ask it again. */
WEAVEC_RT_API const char __weavec_alloc_linked = 1;

#endif
