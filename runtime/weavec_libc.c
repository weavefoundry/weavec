/*===- weavec_libc.c - Checked library calls -----------------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, section 2.5. The guard pass redirects a call whose row names a
|* wrapper to the checked version here: a call whose bytes only the call
|* itself computes (a string copy, a formatted write), which computes them
|* before the call writes; a search or comparison, which checks the bytes it
|* read up to where it stopped; and the mapping calls, which clear the
|* shadow of what they map or unmap.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/types.h>

/* A range guard, inline when one shadow load decides it. */
static inline void checkRange(uintptr_t address, uint64_t n,
                              const struct __weavec_rt_site *site) {
  if (!weavecRtQuickOk(address, n))
    __weavec_rt_range(address, n, site);
}

/* The site of a write: the pass marks the site of the call as a read. */
static struct __weavec_rt_site writeSite(const struct __weavec_rt_site *site) {
  struct __weavec_rt_site write = {0, 0, 0, WeavecRtSiteWrite};
  if (site != NULL) {
    write = *site;
    write.flags |= WeavecRtSiteWrite;
  }
  return write;
}

/* Copies the string at s and its terminator to d, checked; its length. */
static uint64_t copyString(char *d, const char *s,
                           const struct __weavec_rt_site *site) {
  const uint64_t n = __weavec_rt_strlen(s, ~(uint64_t)0, site);
  const struct __weavec_rt_site write = writeSite(site);
  checkRange((uintptr_t)d, n + 1, &write);
  if (d != s && (uintptr_t)d < (uintptr_t)s + n + 1 &&
      (uintptr_t)s < (uintptr_t)d + n + 1)
    __weavec_rt_overlap((uintptr_t)d, (uintptr_t)s, n + 1, &write);
  memmove(d, s, (size_t)n + 1);
  return n;
}

char *__weavec_rt_strcpy(char *d, const char *s,
                         const struct __weavec_rt_site *site) {
  (void)copyString(d, s, site);
  return d;
}

char *__weavec_rt_stpcpy(char *d, const char *s,
                         const struct __weavec_rt_site *site) {
  return d + copyString(d, s, site);
}

char *__weavec_rt_strcat(char *d, const char *s,
                         const struct __weavec_rt_site *site) {
  const uint64_t at = __weavec_rt_strlen(d, ~(uint64_t)0, site);
  const uint64_t n = __weavec_rt_strlen(s, ~(uint64_t)0, site);
  const struct __weavec_rt_site write = writeSite(site);
  checkRange((uintptr_t)d + at, n + 1, &write);
  memcpy(d + at, s, (size_t)n + 1);
  return d;
}

/* How a format reads one argument. */
enum ArgumentKind {
  ArgumentNone,
  ArgumentInt,
  ArgumentLong,
  ArgumentLongLong,
  ArgumentDouble,
  ArgumentLongDouble,
  ArgumentPointer,
  ArgumentString,
  ArgumentTarget
};

enum { FormatArguments = 64 };

struct FormatUse {
  unsigned char kind;
  /* A string's precision: -1 none, else its value, or the argument that
   * holds it (precisionArgument). A target's bytes. */
  int precision;
  int precisionArgument;
  unsigned bytes;
};

/* Reads a decimal number at *at; -1 when there is none. */
static int readNumber(const char **at) {
  int value = -1;
  while (isdigit((unsigned char)**at)) {
    value = (value < 0 ? 0 : value * 10) + (**at - '0');
    ++*at;
    if (value > 100000)
      return -1;
  }
  return value;
}

/* Records that argument `index` (1-based) is read as `kind`; 0 when the
 * format names more arguments than are followed. */
static int use(struct FormatUse *uses, int *count, int index,
               unsigned char kind) {
  if (index < 1 || index > FormatArguments)
    return 0;
  if (uses[index - 1].kind == ArgumentNone)
    uses[index - 1].kind = kind;
  if (index > *count)
    *count = index;
  return 1;
}

/* The memory a format's arguments name: each `%s` string, up to its
 * precision, and each `%n` target, checked before the call reads or writes
 * them. Positional arguments (`%2$s`) are followed too. A format this does
 * not follow (a conversion it does not know, more than 64 arguments) is
 * checked up to there. */
static void checkFormat(const char *format, va_list arguments,
                        const struct __weavec_rt_site *site) {
  struct __weavec_rt_site string = {0, 0, 0, WeavecRtSiteNullOk};
  struct __weavec_rt_site target = writeSite(site);
  struct FormatUse uses[FormatArguments];
  intptr_t values[FormatArguments];
  const char *at;
  int count = 0;
  int next = 1;
  int i;
  va_list walk;
  if (format == NULL)
    return;
  if (site != NULL) {
    string = *site;
    string.flags |= WeavecRtSiteNullOk;
  }
  memset(uses, 0, sizeof uses);
  /* The arguments the format reads, and how. */
  for (at = format; *at != 0; ++at) {
    const char *spec;
    int position;
    int precision = -1;
    int precisionArgument = 0;
    int size = 0; /* 0 int, 1 long, 2 long long, 3 intmax, 4 size,
                     5 ptrdiff, 6 long double, -1 char, -2 short */
    unsigned char kind;
    if (*at != '%')
      continue;
    if (*++at == '%')
      continue;
    spec = at;
    position = readNumber(&at);
    if (position > 0 && *at == '$')
      ++at;
    else {
      at = spec;
      position = 0;
    }
    while (*at != 0 && strchr("-+ #0'", *at) != NULL)
      ++at;
    if (*at == '*') {
      int width = (++at, readNumber(&at));
      if (width > 0 && *at == '$')
        ++at;
      else
        width = next++;
      if (!use(uses, &count, width, ArgumentInt))
        break;
    } else {
      (void)readNumber(&at);
    }
    if (*at == '.') {
      if (*++at == '*') {
        int argument = (++at, readNumber(&at));
        if (argument > 0 && *at == '$')
          ++at;
        else
          argument = next++;
        if (!use(uses, &count, argument, ArgumentInt))
          break;
        precisionArgument = argument;
      } else {
        precision = readNumber(&at);
        if (precision < 0)
          precision = 0;
      }
    }
    switch (*at) {
    case 'h':
      size = *++at == 'h' ? (++at, -1) : -2;
      break;
    case 'l':
      size = *++at == 'l' ? (++at, 2) : 1;
      break;
    case 'j':
      size = 3, ++at;
      break;
    case 'z':
      size = 4, ++at;
      break;
    case 't':
      size = 5, ++at;
      break;
    case 'L':
      size = 6, ++at;
      break;
    default:
      break;
    }
    switch (*at) {
    case 'd': case 'i': case 'o': case 'u': case 'x': case 'X': case 'c':
      kind = size == 2   ? ArgumentLongLong
             : size >= 1 ? ArgumentLong
                         : ArgumentInt;
      break;
    case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a':
    case 'A':
      kind = size == 6 ? ArgumentLongDouble : ArgumentDouble;
      break;
    case 'p':
      kind = ArgumentPointer;
      break;
    case 's':
      /* (A wide string, `%ls`, is not scanned.) */
      kind = size == 1 ? ArgumentPointer : ArgumentString;
      break;
    case 'n':
      kind = ArgumentTarget;
      break;
    default:
      kind = ArgumentNone;
      break;
    }
    if (kind == ArgumentNone)
      break;
    if (position == 0)
      position = next++;
    if (!use(uses, &count, position, kind))
      break;
    uses[position - 1].precision = precision;
    uses[position - 1].precisionArgument = precisionArgument;
    uses[position - 1].bytes = size == -1   ? 1
                               : size == -2 ? (unsigned)sizeof(short)
                               : size == 0  ? (unsigned)sizeof(int)
                                            : (unsigned)sizeof(long long);
    if (*at == 0)
      break;
  }
  /* The arguments in order: each is read as the format reads it, up to the
   * first one the format does not say how to read. */
  va_copy(walk, arguments);
  for (i = 0; i < count; ++i) {
    switch (uses[i].kind) {
    case ArgumentInt:
      values[i] = va_arg(walk, int);
      break;
    case ArgumentLong:
      values[i] = (intptr_t)va_arg(walk, long);
      break;
    case ArgumentLongLong:
      values[i] = (intptr_t)va_arg(walk, long long);
      break;
    case ArgumentDouble:
      (void)va_arg(walk, double);
      break;
    case ArgumentLongDouble:
      (void)va_arg(walk, long double);
      break;
    case ArgumentPointer:
    case ArgumentString:
    case ArgumentTarget:
      values[i] = (intptr_t)va_arg(walk, void *);
      break;
    default:
      count = i;
      break;
    }
  }
  va_end(walk);
  for (i = 0; i < count; ++i) {
    if (uses[i].kind == ArgumentString) {
      int precision = uses[i].precision;
      if (uses[i].precisionArgument > 0) {
        precision = uses[i].precisionArgument <= count
                        ? (int)values[uses[i].precisionArgument - 1]
                        : -1;
      }
      (void)__weavec_rt_strlen(
          (const char *)values[i],
          precision < 0 ? ~(uint64_t)0 : (uint64_t)precision, &string);
    } else if (uses[i].kind == ArgumentTarget) {
      checkRange((uintptr_t)values[i], uses[i].bytes, &target);
    }
  }
}

/* The destinations a scanf-family call wrote: the first `assigned` of its
 * assigning conversions (and each `%n` it reached), checked after the call
 * over the bytes each one wrote. A conversion that did not run wrote
 * nothing. A format this does not follow (positional arguments, a
 * conversion it does not know) is checked up to there. */
static void checkScanned(const char *format, va_list arguments, int assigned,
                         const struct __weavec_rt_site *site) {
  const struct __weavec_rt_site write = writeSite(site);
  const char *at;
  int index = 0;
  va_list walk;
  if (format == NULL)
    return;
  va_copy(walk, arguments);
  for (at = format; *at != 0; ++at) {
    int suppressed = 0;
    int width = -1;
    int size = 0; /* as checkFormat's */
    char conversion;
    void *target;
    uint64_t bytes = 0;
    if (*at != '%')
      continue;
    if (*++at == '%')
      continue;
    if (*at == '*') {
      suppressed = 1;
      ++at;
    }
    width = readNumber(&at);
    if (*at == '$')
      break;
    switch (*at) {
    case 'h':
      size = *++at == 'h' ? (++at, -1) : -2;
      break;
    case 'l':
      size = *++at == 'l' ? (++at, 2) : 1;
      break;
    case 'j': case 'z': case 't':
      size = 3, ++at;
      break;
    case 'L':
      size = 6, ++at;
      break;
    default:
      break;
    }
    conversion = *at;
    if (conversion == '[') {
      /* The set runs to the first `]` after its first character. */
      ++at;
      if (*at == '^')
        ++at;
      if (*at == ']')
        ++at;
      while (*at != 0 && *at != ']')
        ++at;
      if (*at == 0)
        break;
    }
    if (strchr("diouxXaeEfFgGAsc[pn", conversion) == NULL || conversion == 0)
      break;
    if (suppressed)
      continue;
    target = va_arg(walk, void *);
    if (conversion == 'n') {
      /* Reached when every assigning conversion before it ran. */
      if (index > assigned)
        break;
    } else if (index++ >= assigned) {
      break;
    }
    switch (conversion) {
    case 's': case '[':
      if (size != 1 && target != NULL)
        bytes = (uint64_t)strlen((const char *)target) + 1;
      break;
    case 'c':
      if (size != 1)
        bytes = width > 0 ? (uint64_t)width : 1;
      break;
    case 'a': case 'e': case 'E': case 'f': case 'F': case 'g': case 'G':
    case 'A':
      bytes = size == 6   ? sizeof(long double)
              : size == 1 ? sizeof(double)
                          : sizeof(float);
      break;
    case 'p':
      bytes = sizeof(void *);
      break;
    default:
      bytes = size == -1   ? 1
              : size == -2 ? sizeof(short)
              : size == 0  ? sizeof(int)
                           : sizeof(long long);
      break;
    }
    if (bytes != 0)
      checkRange((uintptr_t)target, bytes, &write);
  }
  va_end(walk);
}

int __weavec_rt_vsscanf(const char *s, const char *format, va_list arguments,
                        const struct __weavec_rt_site *site) {
  va_list copy;
  int assigned;
  (void)__weavec_rt_strlen(s, ~(uint64_t)0, site);
  va_copy(copy, arguments);
  assigned = vsscanf(s, format, copy);
  va_end(copy);
  checkScanned(format, arguments, assigned, site);
  return assigned;
}

int __weavec_rt_sscanf(const struct __weavec_rt_site *site, const char *s,
                       const char *format, ...) {
  va_list arguments;
  int assigned;
  va_start(arguments, format);
  assigned = __weavec_rt_vsscanf(s, format, arguments, site);
  va_end(arguments);
  return assigned;
}

int __weavec_rt_vfscanf(FILE *stream, const char *format, va_list arguments,
                        const struct __weavec_rt_site *site) {
  va_list copy;
  int assigned;
  va_copy(copy, arguments);
  assigned = vfscanf(stream, format, copy);
  va_end(copy);
  checkScanned(format, arguments, assigned, site);
  return assigned;
}

int __weavec_rt_fscanf(const struct __weavec_rt_site *site, FILE *stream,
                       const char *format, ...) {
  va_list arguments;
  int assigned;
  va_start(arguments, format);
  assigned = __weavec_rt_vfscanf(stream, format, arguments, site);
  va_end(arguments);
  return assigned;
}

int __weavec_rt_vscanf(const char *format, va_list arguments,
                       const struct __weavec_rt_site *site) {
  return __weavec_rt_vfscanf(stdin, format, arguments, site);
}

int __weavec_rt_scanf(const struct __weavec_rt_site *site, const char *format,
                      ...) {
  va_list arguments;
  int assigned;
  va_start(arguments, format);
  assigned = __weavec_rt_vfscanf(stdin, format, arguments, site);
  va_end(arguments);
  return assigned;
}

int __weavec_rt_vsnprintf(char *d, size_t n, const char *format,
                          va_list arguments,
                          const struct __weavec_rt_site *site) {
  checkFormat(format, arguments, site);
  /* A destination of n addressable bytes takes whatever the call writes,
   * in one formatting pass; otherwise the length the call would write is
   * computed first, and what it writes checked. */
  if (n != 0 && !__weavec_rt_range_ok((uintptr_t)d, n)) {
    va_list copy;
    int length;
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length >= 0) {
      const uint64_t written =
          (uint64_t)length + 1 < n ? (uint64_t)length + 1 : n;
      const struct __weavec_rt_site write = writeSite(site);
      checkRange((uintptr_t)d, written, &write);
    }
  }
  return vsnprintf(d, n, format, arguments);
}

int __weavec_rt_snprintf(const struct __weavec_rt_site *site, char *d,
                         size_t n, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vsnprintf(d, n, format, arguments, site);
  va_end(arguments);
  return result;
}

int __weavec_rt_vsprintf(char *d, const char *format, va_list arguments,
                         const struct __weavec_rt_site *site) {
  va_list copy;
  int length;
  checkFormat(format, arguments, site);
  va_copy(copy, arguments);
  length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0)
    return length;
  {
    const struct __weavec_rt_site write = writeSite(site);
    checkRange((uintptr_t)d, (uint64_t)length + 1, &write);
  }
  return vsnprintf(d, (size_t)length + 1, format, arguments);
}

int __weavec_rt_sprintf(const struct __weavec_rt_site *site, char *d,
                        const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vsprintf(d, format, arguments, site);
  va_end(arguments);
  return result;
}

/* gets, which no length bounds: each byte is checked before it is
 * written. */
char *__weavec_rt_gets(char *d, const struct __weavec_rt_site *site) {
  const struct __weavec_rt_site write = writeSite(site);
  uint64_t n = 0;
  int c;
  while ((c = getchar()) != EOF && c != '\n') {
    checkRange((uintptr_t)d + n, 1, &write);
    d[n++] = (char)c;
  }
  if (c == EOF && (n == 0 || ferror(stdin)))
    return NULL;
  checkRange((uintptr_t)d + n, 1, &write);
  d[n] = 0;
  return d;
}

/* A read of n bytes at s through null, before the call faults on it. */
static void readsNull(const void *s, uint64_t n,
                      const struct __weavec_rt_site *site) {
  if (s == NULL && n != 0)
    weavecRtFail("null-dereference", 0, n, site);
}

void *__weavec_rt_memchr(const void *s, int c, size_t n,
                         const struct __weavec_rt_site *site) {
  void *found;
  readsNull(s, n, site);
  found = memchr(s, c, n);
  checkRange((uintptr_t)s,
                    found != NULL
                        ? (uint64_t)((const char *)found - (const char *)s) + 1
                        : (uint64_t)n,
                    site);
  return found;
}

char *__weavec_rt_strchr(const char *s, int c,
                         const struct __weavec_rt_site *site) {
  char *found;
  readsNull(s, 1, site);
  found = strchr(s, c);
  if (found != NULL)
    checkRange((uintptr_t)s, (uint64_t)(found - s) + 1, site);
  else
    (void)__weavec_rt_strlen(s, ~(uint64_t)0, site);
  return found;
}

/* Compares at most n bytes of a and b, ignoring case when asked, then
 * checks those it read. */
static int compare(const char *a, const char *b, uint64_t n, int folded,
                   const struct __weavec_rt_site *site) {
  uint64_t i = 0;
  int result = 0;
  readsNull(a, n, site);
  readsNull(b, n, site);
  for (; i < n; ++i) {
    const unsigned char x =
        folded ? (unsigned char)tolower((unsigned char)a[i]) : (unsigned char)a[i];
    const unsigned char y =
        folded ? (unsigned char)tolower((unsigned char)b[i]) : (unsigned char)b[i];
    if (x != y || x == 0) {
      result = (int)x - (int)y;
      ++i;
      break;
    }
  }
  checkRange((uintptr_t)a, i, site);
  checkRange((uintptr_t)b, i, site);
  return result;
}

int __weavec_rt_strcmp(const char *a, const char *b,
                       const struct __weavec_rt_site *site) {
  return compare(a, b, ~(uint64_t)0, 0, site);
}

int __weavec_rt_strncmp(const char *a, const char *b, size_t n,
                        const struct __weavec_rt_site *site) {
  return compare(a, b, (uint64_t)n, 0, site);
}

int __weavec_rt_strcasecmp(const char *a, const char *b,
                           const struct __weavec_rt_site *site) {
  return compare(a, b, ~(uint64_t)0, 1, site);
}

int __weavec_rt_strncasecmp(const char *a, const char *b, size_t n,
                            const struct __weavec_rt_site *site) {
  return compare(a, b, (uint64_t)n, 1, site);
}

void *__weavec_rt_mmap(void *address, size_t length, int protection,
                       int flags, int fd, long long offset,
                       const struct __weavec_rt_site *site) {
  void *mapped = mmap(address, length, protection, flags, fd, (off_t)offset);
  (void)site;
  if (mapped != MAP_FAILED)
    weavecRtShadowClear((uintptr_t)mapped, (uintptr_t)mapped + length);
  return mapped;
}

int __weavec_rt_munmap(void *address, size_t length,
                       const struct __weavec_rt_site *site) {
  const int result = munmap(address, length);
  (void)site;
  if (result == 0)
    weavecRtShadowClear((uintptr_t)address, (uintptr_t)address + length);
  return result;
}
