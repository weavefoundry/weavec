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

#if !defined(__APPLE__) || !defined(WEAVEC_RT_ARCHIVE)
/* Amendment 18: a program (or a library it loads) that replaces one of the
 * eight base functions (resolveBases) gets the C++ library's chain instead:
 * each derived form calls its base (`base`, an index of them) as the
 * library's do, and the runtime's base forms allocate as malloc and release
 * any block. */
enum { Own = -1, BaseCount = 8 };
typedef void *NewFunction(size_t, uintptr_t, uintptr_t, uintptr_t);
typedef void DeleteFunction(void *, uintptr_t, uintptr_t);
static void *bases[BaseCount];
static int replacedState; /* 0 not yet known, 1 no, 2 yes */
static int replaced(void);
static void *newSlow(int nothrow, unsigned form, size_t size, size_t align);
static NewFunction *chain(const char *nothrow, int base);

/* `align` and `size` name the argument that carries an alignment, a sized
 * release's size (a, b), or none; the others are ignored. */
#define WEAVEC_NEW(name, form, align, base, nothrow)                           \
  WEAVEC_RT_ALWAYS_WEAK void *name(size_t size, uintptr_t a, uintptr_t b,     \
                                   uintptr_t c);                               \
  void *name(size_t size, uintptr_t a, uintptr_t b, uintptr_t c) {            \
    const unsigned how = replaced() ? WeavecRtFormMalloc : (form);             \
    void *p;                                                                   \
    if ((base) != Own && how == WeavecRtFormMalloc)                            \
      return chain(nothrow ? #name : NULL, (base) + 0)(size, a, b, c);         \
    p = weavecRtAllocForm(size, (size_t)(align), how);                         \
    return p != NULL ? p : newSlow(nothrow, how, size, (size_t)(align));       \
  }
#define WEAVEC_DELETE(name, form, size, base, align)                           \
  WEAVEC_RT_ALWAYS_WEAK void name(void *p, uintptr_t a, uintptr_t b);         \
  void name(void *p, uintptr_t a, uintptr_t b) {                              \
    (void)a, (void)b;                                                          \
    if (!replaced())                                                           \
      weavecRtReleaseForm(p, form, (size_t)(size), RETURN);                    \
    else if ((base) != Own)                                                    \
      ((DeleteFunction *)bases[(base) + 0])(p, (uintptr_t)(align), 0);         \
    else                                                                       \
      weavecRtReleaseForm(p, WeavecRtFormAny, SIZE_MAX, NULL);                 \
  }

enum { New = WeavecRtFormNew, Array = WeavecRtFormNewArray };
#define RETURN __builtin_return_address(0)
WEAVEC_NEW(_Znwm, New, 0, Own, 0)
WEAVEC_NEW(_ZnwmRKSt9nothrow_t, New, 0, 0, 1)
WEAVEC_NEW(_ZnwmSt11align_val_t, New, a, Own, 0)
WEAVEC_NEW(_ZnwmSt11align_val_tRKSt9nothrow_t, New, a, 2, 1)
WEAVEC_NEW(_Znam, Array, 0, 0, 0)
WEAVEC_NEW(_ZnamRKSt9nothrow_t, Array, 0, 1, 1)
WEAVEC_NEW(_ZnamSt11align_val_t, Array, a, 2, 0)
WEAVEC_NEW(_ZnamSt11align_val_tRKSt9nothrow_t, Array, a, 3, 1)
WEAVEC_DELETE(_ZdlPv, New, SIZE_MAX, Own, 0)
WEAVEC_DELETE(_ZdlPvm, New, a, 4, 0)
WEAVEC_DELETE(_ZdlPvSt11align_val_t, New, SIZE_MAX, Own, 0)
WEAVEC_DELETE(_ZdlPvmSt11align_val_t, New, a, 6, b)
WEAVEC_DELETE(_ZdlPvRKSt9nothrow_t, New, SIZE_MAX, 4, 0)
WEAVEC_DELETE(_ZdlPvSt11align_val_tRKSt9nothrow_t, New, SIZE_MAX, 6, a)
WEAVEC_DELETE(_ZdaPv, Array, SIZE_MAX, 4, 0)
WEAVEC_DELETE(_ZdaPvm, Array, a, 5, 0)
WEAVEC_DELETE(_ZdaPvSt11align_val_t, Array, SIZE_MAX, 6, a)
WEAVEC_DELETE(_ZdaPvmSt11align_val_t, Array, a, 7, b)
WEAVEC_DELETE(_ZdaPvRKSt9nothrow_t, Array, SIZE_MAX, 5, 0)
WEAVEC_DELETE(_ZdaPvSt11align_val_tRKSt9nothrow_t, Array, SIZE_MAX, 7, a)

/* The definitions every image resolves the base names to, and whether one
 * is not the runtime's. Linux's library binds its own calls to itself
 * (-Bsymbolic-functions), so the loader is asked; a -static program is one
 * image, whose link resolved them, against the runtime's own (aliases). */
#if defined(WEAVEC_RT_ARCHIVE)
#define OWN(name) static __typeof__(name) own##name __attribute__((alias(#name)));
OWN(_Znwm) OWN(_Znam) OWN(_ZnwmSt11align_val_t) OWN(_ZnamSt11align_val_t)
OWN(_ZdlPv) OWN(_ZdaPv) OWN(_ZdlPvSt11align_val_t) OWN(_ZdaPvSt11align_val_t)
static int resolveBases(void) {
  void *const linked[] = {_Znwm, _Znam, _ZnwmSt11align_val_t,
                          _ZnamSt11align_val_t, _ZdlPv, _ZdaPv,
                          _ZdlPvSt11align_val_t, _ZdaPvSt11align_val_t};
  void *const own[] = {own_Znwm, own_Znam, own_ZnwmSt11align_val_t,
                       own_ZnamSt11align_val_t, own_ZdlPv, own_ZdaPv,
                       own_ZdlPvSt11align_val_t, own_ZdaPvSt11align_val_t};
  int found = 1;
  for (unsigned i = 0; i < BaseCount; ++i)
    bases[i] = linked[i], found = linked[i] != own[i] ? 2 : found;
  return found;
}
#else
static const char *const BaseNames[BaseCount] = {
    "_Znwm", "_Znam", "_ZnwmSt11align_val_t", "_ZnamSt11align_val_t",
    "_ZdlPv", "_ZdaPv", "_ZdlPvSt11align_val_t", "_ZdaPvSt11align_val_t"};

static int resolveBases(void) {
  Dl_info self, info;
  int found = 1;
  (void)dladdr((void *)resolveBases, &self);
  for (unsigned i = 0; i < BaseCount; ++i) {
    bases[i] = dlsym(RTLD_DEFAULT, BaseNames[i]);
    if (bases[i] != NULL && dladdr(bases[i], &info) != 0 &&
        info.dli_fbase != self.dli_fbase)
      found = 2;
  }
  return found;
}
#endif

/* The function a derived form of a replaced base calls: its base, or for a
 * nothrow form the C++ library's own, which catches the base's bad_alloc. */
static NewFunction *chain(const char *nothrow, int base) {
#if !defined(WEAVEC_RT_ARCHIVE)
  NewFunction *library = nothrow ? (NewFunction *)dlsym(RTLD_NEXT, nothrow) : NULL;
  if (library != NULL)
    return library;
#endif
  return (void)nothrow, (NewFunction *)bases[base];
}

static int replaced(void) {
  int state = __atomic_load_n(&replacedState, __ATOMIC_ACQUIRE);
  if (state == 0)
    __atomic_store_n(&replacedState, state = resolveBases(), __ATOMIC_RELEASE);
  return state == 2;
}

/* A form the arena could not serve, as the C++ library's: the new-handler
 * until one succeeds, then null (nothrow) or std::bad_alloc. A -static
 * program's link resolves the library's functions (weak references). */
typedef void Handler(void);
#if defined(WEAVEC_RT_ARCHIVE)
Handler *_ZSt15get_new_handlerv(void) __attribute__((weak));
void _ZSt17__throw_bad_allocv(void) __attribute__((weak));
void _ZNSt3__117__throw_bad_allocEv(void) __attribute__((weak));
#define LIBRARY(name) ((void *)name)
#else
#define LIBRARY(name) dlsym(RTLD_DEFAULT, #name)
#endif

static void *newSlow(int nothrow, unsigned form, size_t size, size_t align) {
  Handler *(*handlerOf)(void) = (Handler * (*)(void)) LIBRARY(_ZSt15get_new_handlerv);
  Handler *fail = (Handler *)LIBRARY(_ZSt17__throw_bad_allocv);
  Handler *handler;
  void *p;
  if (fail == NULL)
    fail = (Handler *)LIBRARY(_ZNSt3__117__throw_bad_allocEv);
  while (!nothrow && handlerOf != NULL && (handler = handlerOf()) != NULL) {
    handler();
    if ((p = weavecRtAllocForm(size, align, form)) != NULL)
      return p;
  }
  if (nothrow)
    return NULL;
  if (fail != NULL)
    fail();
  __weavec_rt_fatal("operator new", NULL, "out of memory");
}
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
