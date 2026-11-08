/*===- weavec_cxx.c - C++: allocation functions, exceptions -------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0039, sections 3, 4 and 6: the replaceable allocation functions, by
|* their Itanium names, with the form of each block; the frames an exception
|* unwound marked returned; the C++ library's failed checks.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <dlfcn.h>
#include <stdarg.h>

/*===-- The allocation functions (section 4) -------------------------------===*/

/* Weak, so that Darwin binds every image's calls to them; a program's wins. */
#define WEAVEC_RT_ALWAYS_WEAK WEAVEC_RT_API __attribute__((weak))

/* `align` and `size` name the argument that carries an alignment, a sized
 * release's size (a, b), or none; the others are ignored. */
#define WEAVEC_NEW(name, form, align)                                          \
  WEAVEC_RT_ALWAYS_WEAK void *name(size_t size, uintptr_t a, uintptr_t b,     \
                                   uintptr_t c);                               \
  void *name(size_t size, uintptr_t a, uintptr_t b, uintptr_t c) {            \
    void *p = weavecRtAllocForm(size, (size_t)(align), form);                  \
    return p != NULL ? p : newSlow(#name, name, form, size, a, b, c);          \
  }
#define WEAVEC_DELETE(name, form, size)                                        \
  WEAVEC_RT_ALWAYS_WEAK void name(void *p, uintptr_t a, uintptr_t b);         \
  void name(void *p, uintptr_t a, uintptr_t b) {                              \
    (void)a, (void)b;                                                          \
    weavecRtReleaseForm(p, form, (size_t)(size), RETURN);                      \
  }

enum { New = WeavecRtFormNew, Array = WeavecRtFormNewArray };
#define RETURN __builtin_return_address(0)
#if !defined(__APPLE__) || !defined(WEAVEC_RT_ARCHIVE)
/* A form the arena could not serve: the C++ library's own runs the
 * new-handler and throws std::bad_alloc (its nothrow form returns null). */
typedef void *LibraryNew(size_t, uintptr_t, uintptr_t, uintptr_t);

static void *newSlow(const char *name, LibraryNew *self, unsigned form,
                     size_t size, uintptr_t a, uintptr_t b, uintptr_t c) {
  LibraryNew *next = (LibraryNew *)dlsym(RTLD_NEXT, name);
  void *p;
  if (next == NULL || next == self)
    __weavec_rt_fatal("operator new", NULL, "out of memory");
  p = next(size, a, b, c);
  if (p != NULL)
    weavecRtSetForm(p, form);
  return p;
}

WEAVEC_NEW(_Znwm, New, 0)
WEAVEC_NEW(_ZnwmRKSt9nothrow_t, New, 0)
WEAVEC_NEW(_ZnwmSt11align_val_t, New, a)
WEAVEC_NEW(_ZnwmSt11align_val_tRKSt9nothrow_t, New, a)
WEAVEC_NEW(_Znam, Array, 0)
WEAVEC_NEW(_ZnamRKSt9nothrow_t, Array, 0)
WEAVEC_NEW(_ZnamSt11align_val_t, Array, a)
WEAVEC_NEW(_ZnamSt11align_val_tRKSt9nothrow_t, Array, a)
WEAVEC_DELETE(_ZdlPv, New, SIZE_MAX)
WEAVEC_DELETE(_ZdlPvm, New, a)
WEAVEC_DELETE(_ZdlPvSt11align_val_t, New, SIZE_MAX)
WEAVEC_DELETE(_ZdlPvmSt11align_val_t, New, a)
WEAVEC_DELETE(_ZdlPvRKSt9nothrow_t, New, SIZE_MAX)
WEAVEC_DELETE(_ZdlPvSt11align_val_tRKSt9nothrow_t, New, SIZE_MAX)
WEAVEC_DELETE(_ZdaPv, Array, SIZE_MAX)
WEAVEC_DELETE(_ZdaPvm, Array, a)
WEAVEC_DELETE(_ZdaPvSt11align_val_t, Array, SIZE_MAX)
WEAVEC_DELETE(_ZdaPvmSt11align_val_t, Array, a)
WEAVEC_DELETE(_ZdaPvRKSt9nothrow_t, Array, SIZE_MAX)
WEAVEC_DELETE(_ZdaPvSt11align_val_tRKSt9nothrow_t, Array, SIZE_MAX)
#endif

/*===-- Exceptions (section 3) ---------------------------------------------===*/

/* The lowest unmarked stack address of the exception in flight, or 0. All
 * below the marking call's frame is dead; a stale mark `settle` clears. */
static __thread uintptr_t unwindLow;

void weavecRtRaising(uintptr_t frame) {
  unwindLow = unwindLow == 0 || frame < unwindLow ? frame : unwindLow;
}

/* Marks [unwindLow, high) of the thread's own stack returned. */
static void markUnwound(uintptr_t high) {
  const uintptr_t low = unwindLow & ~(uintptr_t)15;
  high &= ~(uintptr_t)15;
  if (low == 0 || high <= low || low < weavecRtStackBottom() ||
      high > weavecRtStackTop())
    return;
  weavecRtShadowSet(low, high - low, WeavecRtShadowReturned);
}

void __weavec_rt_landed(void) {
  const uintptr_t frame = (uintptr_t)__builtin_frame_address(0);
  markUnwound(frame);
  /* (A cleanup resumes: the frames above are marked from here.) */
  unwindLow = unwindLow != 0 && unwindLow < frame ? frame : unwindLow;
}

void weavecRtCaught(uintptr_t frame) { markUnwound(frame), unwindLow = 0; }

/*===-- The C++ library's checks (section 6) -------------------------------===*/

void __weavec_rt_library_assertion(const char *message,
                                   const struct __weavec_rt_site *site) {
  weavecRtFailMessage("library-assertion", message, site);
}

void __weavec_rt_glibcxx_assert_fail(const char *file, int line,
                                     const char *function,
                                     const char *condition,
                                     const struct __weavec_rt_site *site) {
  (void)file, (void)line, (void)function;
  weavecRtFailMessage("library-assertion",
                      condition != NULL ? condition : "", site);
}

void __weavec_rt_libcpp_verbose_abort(const struct __weavec_rt_site *site,
                                      const char *format, ...) {
  char message[512];
  size_t length;
  va_list arguments;
  va_start(arguments, format);
  (void)vsnprintf(message, sizeof message, format, arguments);
  va_end(arguments);
  for (length = strlen(message); length != 0 && message[length - 1] == '\n';)
    message[--length] = 0;
  weavecRtFailMessage("library-assertion", message, site);
}
