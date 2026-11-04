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
|* shadow of what they map or unmap. RFC 0036 section 6: a writing call is
|* also checked against the object size of its fortified form (`limit`), and
|* the number parsers check what they read after the call.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#if defined(__APPLE__)
#include <libgen.h>
#include <sys/param.h>
#include <sys/sysctl.h>
#include <xlocale.h>
#endif

/* A range guard, inline when one shadow load decides it. */
static inline void checkRange(uintptr_t address, uint64_t n,
                              const struct __weavec_rt_site *site) {
  if (n == 0)
    return;
  if (!weavecRtQuickOk(address, n))
    __weavec_rt_range(address, n, site);
}

/* A fortified call's object size: no more than `limit` bytes at d. */
static inline void checkLimit(const char *d, uint64_t n, size_t limit,
                              const struct __weavec_rt_site *write) {
  if (n > limit)
    __weavec_rt_limit_fail((uintptr_t)d, n, limit, write);
}

/* The site of a write: the pass marks the site of the call as a read. */
/* (Filled in place: a site's file is relative to the record, so a copy
 * returned by value would lose it.) */
static void writeSite(struct __weavec_rt_site *write,
                      const struct __weavec_rt_site *site) {
  weavecRtSiteSet(write, weavecRtSiteFile(site), site ? site->line : 0,
                  site ? site->column : 0,
                  (site ? site->flags : 0) | WeavecRtSiteWrite);
}

/* Copies the string at s and its terminator to d, checked; its length. */
static uint64_t copyString(char *d, const char *s, size_t limit,
                           const struct __weavec_rt_site *site) {
  const uint64_t n = __weavec_rt_strlen(s, ~(uint64_t)0, site);
  struct __weavec_rt_site write;
  writeSite(&write, site);
  checkRange((uintptr_t)s, n + 1, site);
  checkRange((uintptr_t)d, n + 1, &write);
  checkLimit(d, n + 1, limit, &write);
  if (d != s && (uintptr_t)d < (uintptr_t)s + n + 1 &&
      (uintptr_t)s < (uintptr_t)d + n + 1)
    __weavec_rt_overlap((uintptr_t)d, (uintptr_t)s, n + 1, &write);
  memmove(d, s, (size_t)n + 1);
  return n;
}

char *__weavec_rt_strcpy(char *d, const char *s, size_t limit,
                         const struct __weavec_rt_site *site) {
  (void)copyString(d, s, limit, site);
  return d;
}

char *__weavec_rt_stpcpy(char *d, const char *s, size_t limit,
                         const struct __weavec_rt_site *site) {
  return d + copyString(d, s, limit, site);
}

char *__weavec_rt_strcat(char *d, const char *s, size_t limit,
                         const struct __weavec_rt_site *site) {
  const uint64_t at = __weavec_rt_strlen(d, ~(uint64_t)0, site);
  const uint64_t n = __weavec_rt_strlen(s, ~(uint64_t)0, site);
  struct __weavec_rt_site write;
  writeSite(&write, site);
  checkRange((uintptr_t)d, at + n + 1, &write);
  checkLimit(d, at + n + 1, limit, &write);
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
  struct __weavec_rt_site target;
  struct FormatUse uses[FormatArguments];
  intptr_t values[FormatArguments];
  const char *at;
  int count = 0;
  int next = 1;
  int i;
  va_list walk;
  if (format == NULL)
    return;
  writeSite(&target, site);
  if (site != NULL)
    weavecRtSiteSet(&string, weavecRtSiteFile(site), site->line, site->column,
                    site->flags | WeavecRtSiteNullOk);
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
  struct __weavec_rt_site write;
  writeSite(&write, site);
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
                          va_list arguments, size_t limit,
                          const struct __weavec_rt_site *site) {
  checkFormat(format, arguments, site);
  /* A destination of n addressable bytes within its limit takes whatever
   * the call writes, in one formatting pass; otherwise the length the call
   * would write is computed first, and what it writes checked. */
  if (n != 0 && (!__weavec_rt_range_ok((uintptr_t)d, n) || n > limit)) {
    va_list copy;
    int length;
    va_copy(copy, arguments);
    length = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (length >= 0) {
      const uint64_t written =
          (uint64_t)length + 1 < n ? (uint64_t)length + 1 : n;
      struct __weavec_rt_site write;
  writeSite(&write, site);
      checkRange((uintptr_t)d, written, &write);
      checkLimit(d, written, limit, &write);
    }
  }
  return vsnprintf(d, n, format, arguments);
}

int __weavec_rt_snprintf(const struct __weavec_rt_site *site, size_t limit,
                         char *d, size_t n, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vsnprintf(d, n, format, arguments, limit, site);
  va_end(arguments);
  return result;
}

int __weavec_rt_vsprintf(char *d, const char *format, va_list arguments,
                         size_t limit, const struct __weavec_rt_site *site) {
  va_list copy;
  int length;
  checkFormat(format, arguments, site);
  va_copy(copy, arguments);
  length = vsnprintf(NULL, 0, format, copy);
  va_end(copy);
  if (length < 0)
    return length;
  {
    struct __weavec_rt_site write;
  writeSite(&write, site);
    checkRange((uintptr_t)d, (uint64_t)length + 1, &write);
    checkLimit(d, (uint64_t)length + 1, limit, &write);
  }
  return vsnprintf(d, (size_t)length + 1, format, arguments);
}

int __weavec_rt_sprintf(const struct __weavec_rt_site *site, size_t limit,
                        char *d, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vsprintf(d, format, arguments, limit, site);
  va_end(arguments);
  return result;
}

/* gets, which no length bounds: each byte is checked before it is
 * written. */
char *__weavec_rt_gets(char *d, size_t limit,
                       const struct __weavec_rt_site *site) {
  struct __weavec_rt_site write;
  writeSite(&write, site);
  uint64_t n = 0;
  int c;
  /* Byte by byte, each next to the last: the first that is not addressable
   * is the report's. */
  while ((c = getchar()) != EOF && c != '\n') {
    checkRange((uintptr_t)d + n, 1, &write);
    checkLimit(d, n + 1, limit, &write);
    d[n++] = (char)c;
  }
  if (c == EOF && (n == 0 || ferror(stdin)))
    return NULL;
  checkRange((uintptr_t)d + n, 1, &write);
  checkLimit(d, n + 1, limit, &write);
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

/*===-- Number parsing (RFC 0036 section 6) --------------------------------===*/

/* Checks the bytes a number parser read from s, which stopped at end, and
 * stores end through the caller's end pointer. */
static void parsed(const char *s, char *end, char **out,
                   const struct __weavec_rt_site *site) {
  uint64_t read;
  if (end != s) {
    read = (uint64_t)(end - s) + 1;
  } else {
    /* Nothing converted: the white space and sign skipped, and the
     * character that refused. */
    const char *at = s;
    while (isspace((unsigned char)*at))
      ++at;
    if (*at == '+' || *at == '-')
      ++at;
    read = (uint64_t)(at - s) + 1;
  }
  checkRange((uintptr_t)s, read, site);
  if (out != NULL) {
    struct __weavec_rt_site write;
  writeSite(&write, site);
    checkRange((uintptr_t)out, sizeof *out, &write);
    *out = end;
  }
}

#define WEAVEC_RT_PARSER(type, name, call)                                     \
  type __weavec_rt_##name(const char *s, char **end, int base,                 \
                          const struct __weavec_rt_site *site) {               \
    char *stop = (char *)s;                                                    \
    type value;                                                                \
    readsNull(s, 1, site);                                                     \
    value = call(s, &stop, base);                                              \
    parsed(s, stop, end, site);                                                \
    return value;                                                              \
  }

WEAVEC_RT_PARSER(long, strtol, strtol)
WEAVEC_RT_PARSER(long long, strtoll, strtoll)
WEAVEC_RT_PARSER(unsigned long, strtoul, strtoul)
WEAVEC_RT_PARSER(unsigned long long, strtoull, strtoull)
WEAVEC_RT_PARSER(intmax_t, strtoimax, strtoimax)
WEAVEC_RT_PARSER(uintmax_t, strtoumax, strtoumax)

#define WEAVEC_RT_FLOAT_PARSER(type, name, call)                               \
  type __weavec_rt_##name(const char *s, char **end,                           \
                          const struct __weavec_rt_site *site) {               \
    char *stop = (char *)s;                                                    \
    type value;                                                                \
    readsNull(s, 1, site);                                                     \
    value = call(s, &stop);                                                    \
    parsed(s, stop, end, site);                                                \
    return value;                                                              \
  }

WEAVEC_RT_FLOAT_PARSER(double, strtod, strtod)
WEAVEC_RT_FLOAT_PARSER(float, strtof, strtof)
WEAVEC_RT_FLOAT_PARSER(long double, strtold, strtold)

int __weavec_rt_atoi(const char *s, const struct __weavec_rt_site *site) {
  return (int)__weavec_rt_strtol(s, NULL, 10, site);
}

long __weavec_rt_atol(const char *s, const struct __weavec_rt_site *site) {
  return __weavec_rt_strtol(s, NULL, 10, site);
}

long long __weavec_rt_atoll(const char *s,
                            const struct __weavec_rt_site *site) {
  return __weavec_rt_strtoll(s, NULL, 10, site);
}

double __weavec_rt_atof(const char *s, const struct __weavec_rt_site *site) {
  return __weavec_rt_strtod(s, NULL, site);
}

/*===-- Bytes a C type or a structure gives (RFC 0037 section 5) -----------===*/

/* The calls whose bytes are a type's size (the runtime is compiled with the
 * target's headers), a length read through a pointer, or the buffers an
 * array of iovec names: checked before the call, and the lengths it reports
 * writing after it. */

static void checkWrite(const void *p, uint64_t n,
                       const struct __weavec_rt_site *site) {
  struct __weavec_rt_site write;
  writeSite(&write, site);
  checkRange((uintptr_t)p, n, &write);
}

static void checkVectors(const struct iovec *v, int count, int write,
                         const struct __weavec_rt_site *site) {
  int i;
  if (count <= 0)
    return;
  checkRange((uintptr_t)v, (uint64_t)count * sizeof *v, site);
  for (i = 0; i < count; ++i) {
    if (write)
      checkWrite(v[i].iov_base, v[i].iov_len, site);
    else
      checkRange((uintptr_t)v[i].iov_base, v[i].iov_len, site);
  }
}

long __weavec_rt_readv(int fd, const struct iovec *v, int count,
                       const struct __weavec_rt_site *site) {
  checkVectors(v, count, 1, site);
  return readv(fd, v, count);
}

long __weavec_rt_writev(int fd, const struct iovec *v, int count,
                        const struct __weavec_rt_site *site) {
  checkVectors(v, count, 0, site);
  return writev(fd, v, count);
}

long __weavec_rt_preadv(int fd, const struct iovec *v, int count,
                        int64_t offset, const struct __weavec_rt_site *site) {
  checkVectors(v, count, 1, site);
  return preadv(fd, v, count, (off_t)offset);
}

long __weavec_rt_pwritev(int fd, const struct iovec *v, int count,
                         int64_t offset, const struct __weavec_rt_site *site) {
  checkVectors(v, count, 0, site);
  return pwritev(fd, v, count, (off_t)offset);
}

/* A message's header, its address, its vectors and its control data. */
static void checkMessage(const struct msghdr *m, int write,
                         const struct __weavec_rt_site *site) {
  if (write)
    checkWrite(m, sizeof *m, site);
  else
    checkRange((uintptr_t)m, sizeof *m, site);
  if (m->msg_name != NULL) {
    if (write)
      checkWrite(m->msg_name, m->msg_namelen, site);
    else
      checkRange((uintptr_t)m->msg_name, m->msg_namelen, site);
  }
  checkVectors(m->msg_iov, (int)m->msg_iovlen, write, site);
  if (m->msg_control != NULL) {
    if (write)
      checkWrite(m->msg_control, m->msg_controllen, site);
    else
      checkRange((uintptr_t)m->msg_control, m->msg_controllen, site);
  }
}

long __weavec_rt_recvmsg(int fd, struct msghdr *m, int flags,
                         const struct __weavec_rt_site *site) {
  checkMessage(m, 1, site);
  return recvmsg(fd, m, flags);
}

long __weavec_rt_sendmsg(int fd, const struct msghdr *m, int flags,
                         const struct __weavec_rt_site *site) {
  checkMessage(m, 0, site);
  return sendmsg(fd, m, flags);
}

/* A line buffer of the capacity the caller states (the call may write all
 * of it before it grows it). */
static void checkLine(char **line, size_t *n,
                      const struct __weavec_rt_site *site) {
  checkWrite(line, sizeof *line, site);
  checkWrite(n, sizeof *n, site);
  if (*line != NULL && *n != 0)
    checkWrite(*line, *n, site);
}

long __weavec_rt_getline(char **line, size_t *n, FILE *stream,
                         const struct __weavec_rt_site *site) {
  checkLine(line, n, site);
  return getline(line, n, stream);
}

long __weavec_rt_getdelim(char **line, size_t *n, int delimiter, FILE *stream,
                          const struct __weavec_rt_site *site) {
  checkLine(line, n, site);
  return getdelim(line, n, delimiter, stream);
}

/* The bytes of an address of the family (4 or 16), or 0 for another. */
static uint64_t familyBytes(int family) {
  return family == AF_INET ? 4 : family == AF_INET6 ? 16 : 0;
}

int __weavec_rt_inet_pton(int family, const char *text, void *address,
                          const struct __weavec_rt_site *site) {
  (void)__weavec_rt_strlen(text, ~(uint64_t)0, site);
  checkWrite(address, familyBytes(family), site);
  return inet_pton(family, text, address);
}

const char *__weavec_rt_inet_ntop(int family, const void *address, char *text,
                                  unsigned size,
                                  const struct __weavec_rt_site *site) {
  checkRange((uintptr_t)address, familyBytes(family), site);
  checkWrite(text, size, site);
  return inet_ntop(family, address, text, (socklen_t)size);
}

int __weavec_rt_stat(const char *path, struct stat *out,
                     const struct __weavec_rt_site *site) {
  (void)__weavec_rt_strlen(path, ~(uint64_t)0, site);
  checkWrite(out, sizeof *out, site);
  return stat(path, out);
}

int __weavec_rt_lstat(const char *path, struct stat *out,
                      const struct __weavec_rt_site *site) {
  (void)__weavec_rt_strlen(path, ~(uint64_t)0, site);
  checkWrite(out, sizeof *out, site);
  return lstat(path, out);
}

int __weavec_rt_fstat(int fd, struct stat *out,
                      const struct __weavec_rt_site *site) {
  checkWrite(out, sizeof *out, site);
  return fstat(fd, out);
}

int __weavec_rt_fstatat(int fd, const char *path, struct stat *out, int flags,
                        const struct __weavec_rt_site *site) {
  (void)__weavec_rt_strlen(path, ~(uint64_t)0, site);
  checkWrite(out, sizeof *out, site);
  return fstatat(fd, path, out, flags);
}

struct tm *__weavec_rt_gmtime_r(const long *time, struct tm *out,
                                const struct __weavec_rt_site *site) {
  checkRange((uintptr_t)time, sizeof(time_t), site);
  checkWrite(out, sizeof *out, site);
  return gmtime_r((const time_t *)time, out);
}

struct tm *__weavec_rt_localtime_r(const long *time, struct tm *out,
                                   const struct __weavec_rt_site *site) {
  checkRange((uintptr_t)time, sizeof(time_t), site);
  checkWrite(out, sizeof *out, site);
  return localtime_r((const time_t *)time, out);
}

int __weavec_rt_uname(struct utsname *out,
                      const struct __weavec_rt_site *site) {
  checkWrite(out, sizeof *out, site);
  return uname(out);
}

/* A buffer whose length the caller passes through `length` (a socklen_t
 * the call reads and writes). */
static void checkLength(const void *buffer, const unsigned *length,
                        const struct __weavec_rt_site *site) {
  if (buffer == NULL || length == NULL)
    return;
  checkWrite(length, sizeof *length, site);
  checkWrite(buffer, *length, site);
}

int __weavec_rt_getsockopt(int fd, int level, int name, void *value,
                           unsigned *length,
                           const struct __weavec_rt_site *site) {
  checkLength(value, length, site);
  return getsockopt(fd, level, name, value, (socklen_t *)length);
}

int __weavec_rt_getsockname(int fd, struct sockaddr *address, unsigned *length,
                            const struct __weavec_rt_site *site) {
  checkLength(address, length, site);
  return getsockname(fd, address, (socklen_t *)length);
}

int __weavec_rt_getpeername(int fd, struct sockaddr *address, unsigned *length,
                            const struct __weavec_rt_site *site) {
  checkLength(address, length, site);
  return getpeername(fd, address, (socklen_t *)length);
}

int __weavec_rt_accept(int fd, struct sockaddr *address, unsigned *length,
                       const struct __weavec_rt_site *site) {
  checkLength(address, length, site);
  return accept(fd, address, (socklen_t *)length);
}

long __weavec_rt_recvfrom(int fd, void *buffer, size_t n, int flags,
                          struct sockaddr *address, unsigned *length,
                          size_t limit, const struct __weavec_rt_site *site) {
  struct __weavec_rt_site write;
  writeSite(&write, site);
  checkWrite(buffer, n, site);
  checkLimit((const char *)buffer, n, limit, &write);
  checkLength(address, length, site);
  return recvfrom(fd, buffer, n, flags, address, (socklen_t *)length);
}

char *__weavec_rt_if_indextoname(unsigned index, char *name,
                                 const struct __weavec_rt_site *site) {
  checkWrite(name, IF_NAMESIZE, site);
  return if_indextoname(index, name);
}

/* The narrow printf family, whose format may not be a constant: its %s and
 * %n arguments are checked as the format is read. */

int __weavec_rt_vprintf(const char *format, va_list arguments,
                        const struct __weavec_rt_site *site) {
  checkFormat(format, arguments, site);
  return vprintf(format, arguments);
}

int __weavec_rt_printf(const struct __weavec_rt_site *site, const char *format,
                       ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vprintf(format, arguments, site);
  va_end(arguments);
  return result;
}

int __weavec_rt_vfprintf(FILE *stream, const char *format, va_list arguments,
                         const struct __weavec_rt_site *site) {
  checkFormat(format, arguments, site);
  return vfprintf(stream, format, arguments);
}

int __weavec_rt_fprintf(const struct __weavec_rt_site *site, FILE *stream,
                        const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vfprintf(stream, format, arguments, site);
  va_end(arguments);
  return result;
}

int __weavec_rt_vdprintf(int fd, const char *format, va_list arguments,
                         const struct __weavec_rt_site *site) {
  checkFormat(format, arguments, site);
  return vdprintf(fd, format, arguments);
}

int __weavec_rt_dprintf(const struct __weavec_rt_site *site, int fd,
                        const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vdprintf(fd, format, arguments, site);
  va_end(arguments);
  return result;
}

int __weavec_rt_vasprintf(char **out, const char *format, va_list arguments,
                          const struct __weavec_rt_site *site) {
  checkWrite(out, sizeof *out, site);
  checkFormat(format, arguments, site);
  return vasprintf(out, format, arguments);
}

int __weavec_rt_asprintf(const struct __weavec_rt_site *site, char **out,
                         const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vasprintf(out, format, arguments, site);
  va_end(arguments);
  return result;
}

#if defined(__APPLE__)

int __weavec_rt_sysctl(int *name, unsigned count, void *old, size_t *length,
                       void *new_, size_t newLength,
                       const struct __weavec_rt_site *site) {
  checkRange((uintptr_t)name, (uint64_t)count * sizeof *name, site);
  if (old != NULL && length != NULL) {
    checkWrite(length, sizeof *length, site);
    checkWrite(old, *length, site);
  }
  if (new_ != NULL)
    checkRange((uintptr_t)new_, newLength, site);
  return sysctl(name, count, old, length, new_, newLength);
}

int __weavec_rt_sysctlbyname(const char *name, void *old, size_t *length,
                             void *new_, size_t newLength,
                             const struct __weavec_rt_site *site) {
  (void)__weavec_rt_strlen(name, ~(uint64_t)0, site);
  if (old != NULL && length != NULL) {
    checkWrite(length, sizeof *length, site);
    checkWrite(old, *length, site);
  }
  if (new_ != NULL)
    checkRange((uintptr_t)new_, newLength, site);
  return sysctlbyname(name, old, length, new_, newLength);
}

char *__weavec_rt_basename_r(const char *path, char *base,
                             const struct __weavec_rt_site *site) {
  if (path != NULL)
    (void)__weavec_rt_strlen(path, ~(uint64_t)0, site);
  checkWrite(base, MAXPATHLEN, site);
  return basename_r(path, base);
}

char *__weavec_rt_dirname_r(const char *path, char *base,
                            const struct __weavec_rt_site *site) {
  if (path != NULL)
    (void)__weavec_rt_strlen(path, ~(uint64_t)0, site);
  checkWrite(base, MAXPATHLEN, site);
  return dirname_r(path, base);
}

/* Darwin's xlocale forms of the formatted writes. */
int __weavec_rt_vsnprintf_l(char *d, size_t n, void *locale, const char *format,
                            va_list arguments,
                            const struct __weavec_rt_site *site) {
  checkFormat(format, arguments, site);
  if (n != 0) {
    va_list copy;
    int length;
    va_copy(copy, arguments);
    length = vsnprintf_l(NULL, 0, (locale_t)locale, format, copy);
    va_end(copy);
    if (length >= 0)
      checkWrite(d, (uint64_t)length + 1 < n ? (uint64_t)length + 1 : n, site);
  }
  return vsnprintf_l(d, n, (locale_t)locale, format, arguments);
}

int __weavec_rt_snprintf_l(const struct __weavec_rt_site *site, char *d,
                           size_t n, void *locale, const char *format, ...) {
  va_list arguments;
  int result;
  va_start(arguments, format);
  result = __weavec_rt_vsnprintf_l(d, n, locale, format, arguments, site);
  va_end(arguments);
  return result;
}

int __weavec_rt_sprintf_l(const struct __weavec_rt_site *site, char *d,
                          void *locale, const char *format, ...) {
  va_list arguments;
  va_list copy;
  int length;
  va_start(arguments, format);
  checkFormat(format, arguments, site);
  va_copy(copy, arguments);
  length = vsnprintf_l(NULL, 0, (locale_t)locale, format, copy);
  va_end(copy);
  if (length >= 0) {
    checkWrite(d, (uint64_t)length + 1, site);
    length = vsnprintf_l(d, (size_t)length + 1, (locale_t)locale, format,
                         arguments);
  }
  va_end(arguments);
  return length;
}

#endif

/* RFC 0037 section 5: a wide string's length in elements, at most `max`,
 * each element checked before it is read, so that an unterminated string
 * is reported before a byte past its object is read (wide strings are
 * rare; a reported one is then measured as the call would). */
uint64_t __weavec_rt_wcslen(const wchar_t *s, uint64_t max,
                            const struct __weavec_rt_site *site) {
  uint64_t length;
  if (s == NULL) {
    if (site == NULL || (site->flags & WeavecRtSiteNullOk) == 0)
      weavecRtFail("null-dereference", 0, sizeof *s, site);
    return 0;
  }
  for (length = 0; length < max; ++length) {
    const uintptr_t at = (uintptr_t)(s + length);
    if (!weavecRtQuickOk(at, sizeof *s) &&
        !__weavec_rt_range_ok(at, sizeof *s)) {
      weavecRtFail("unterminated-string", (uintptr_t)s,
                   (length + 1) * sizeof *s, site);
      return length + wcsnlen(s + length, max - length > (size_t)-1
                                              ? (size_t)-1
                                              : (size_t)(max - length));
    }
    if (s[length] == 0)
      return length;
  }
  return max;
}
