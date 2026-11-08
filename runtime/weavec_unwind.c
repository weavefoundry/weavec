/*===- weavec_unwind.c - The unwinding interposers -----------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0039, section 3: where the frames an exception unwinds begin (every
|* throw raises through _Unwind_RaiseException or _Unwind_Resume_or_Rethrow)
|* and end (every catch begins with __cxa_begin_catch). Darwin: __interpose
|* entries; Linux: definitions ahead of the unwinder's; -static (the
|* archive's copy, WEAVEC_RT_ARCHIVE): the --wrap weavec-cc passes.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <dlfcn.h>

#define FRAME ((uintptr_t)__builtin_frame_address(0))

#if defined(__APPLE__) || defined(WEAVEC_RT_ARCHIVE)
#if defined(__APPLE__)
#define REAL(name) name
#define OURS(name) weavec_##name
#else
#define REAL(name) __real_##name
#define OURS(name) __wrap_##name
#endif
extern int REAL(_Unwind_RaiseException)(void *), OURS(_Unwind_RaiseException)(void *);
extern int REAL(_Unwind_Resume_or_Rethrow)(void *),
    OURS(_Unwind_Resume_or_Rethrow)(void *);
extern void *REAL(__cxa_begin_catch)(void *), *OURS(__cxa_begin_catch)(void *);
#define NEXT(name, type) REAL(name)
#else
WEAVEC_RT_API int _Unwind_RaiseException(void *exception);
WEAVEC_RT_API int _Unwind_Resume_or_Rethrow(void *exception);
WEAVEC_RT_API void *__cxa_begin_catch(void *exception);
#define OURS(name) name
#define NEXT(name, type) ((type)next(#name))

/* The definition after the runtime's: the unwinder's, the C++ library's. */
static void *next(const char *name) {
  void *found = dlsym(RTLD_NEXT, name);
  if (found == NULL)
    __weavec_rt_fatal("an exception", NULL, "the process has no unwinder");
  return found;
}
#endif

int OURS(_Unwind_RaiseException)(void *exception) {
  weavecRtRaising(FRAME);
  return NEXT(_Unwind_RaiseException, int (*)(void *))(exception);
}

int OURS(_Unwind_Resume_or_Rethrow)(void *exception) {
  weavecRtRaising(FRAME);
  return NEXT(_Unwind_Resume_or_Rethrow, int (*)(void *))(exception);
}

void *OURS(__cxa_begin_catch)(void *exception) {
  void *object = NEXT(__cxa_begin_catch, void *(*)(void *))(exception);
  weavecRtCaught(FRAME);
  return object;
}

#if defined(__APPLE__) && !defined(WEAVEC_RT_ARCHIVE)
#define INTERPOSE(name) {(const void *)OURS(name), (const void *)name}
__attribute__((used, section("__DATA,__interpose"))) static const struct {
  const void *ours, *real;
} Interposers[] = {INTERPOSE(_Unwind_RaiseException),
                   INTERPOSE(_Unwind_Resume_or_Rethrow),
                   INTERPOSE(__cxa_begin_catch)};
#endif
