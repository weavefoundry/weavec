/*===- weavec_report.c - Failed checks: reports and fatal errors ---*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0030, section 10.7, and RFC 0032, section 3. Under
|* -fweavec-checks=report a failed check or guard calls __weavec_rt_report
|* instead of trapping, and the program goes on, with no guarantee. The
|* runtime prints
|*
|*   weavec: runtime check failed: <template> at <file>:<line>:<column>
|*
|* to stderr once per site, where <template> is nonnull, index, span, len,
|* disjoint, assert, violation, object, live or release. With
|* WEAVEC_RT_ABORT=1 in the environment it prints the line and aborts. The
|* case runner and the corpus gate attribute traps to lines and templates
|* through this output.
|*
|* __weavec_rt_fatal is how the allocator stops a release no guard saw (in a
|* unit built without WeaveC, or at a proven facet): it prints
|*
|*   weavec: <what> of <pointer>: <why>
|*
|* and traps. It uses no allocation and no stdio.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <sys/auxv.h>
#endif

/* The sites already reported, as 64-bit hashes of (template, file, line,
 * column) in an open-addressing table; 0 marks a free slot. When the table
 * is full every further failure is printed, which is harmless. */
enum { SiteSlots = 4096 };
static unsigned long long reportedSites[SiteSlots];

static unsigned long long hashText(unsigned long long hash, const char *text) {
  const unsigned char *byte = (const unsigned char *)(text ? text : "");
  for (; *byte; ++byte)
    hash = (hash ^ *byte) * 1099511628211ULL;
  return (hash ^ 0xffU) * 1099511628211ULL;
}

static unsigned long long siteHash(const char *check, const char *file,
                                   unsigned line, unsigned column) {
  unsigned long long hash = hashText(14695981039346656037ULL, check);
  hash = hashText(hash, file);
  hash = (hash ^ line) * 1099511628211ULL;
  hash = (hash ^ column) * 1099511628211ULL;
  return hash ? hash : 1;
}

/* True the first time a site is seen; safe under concurrent reports. */
static int firstReport(unsigned long long hash) {
  unsigned probe;
  for (probe = 0; probe < SiteSlots; ++probe) {
    unsigned long long *slot = &reportedSites[(hash + probe) % SiteSlots];
    unsigned long long seen = __atomic_load_n(slot, __ATOMIC_ACQUIRE);
    if (seen == 0) {
      if (__atomic_compare_exchange_n(slot, &seen, hash, 0, __ATOMIC_ACQ_REL,
                                      __ATOMIC_ACQUIRE))
        return 1;
    }
    if (seen == hash)
      return 0;
  }
  return 1;
}

/* WEAVEC_RT_ABORT=1, read once: -1 unknown, 0 no, 1 yes. */
static int abortSetting = -1;

static int shouldAbort(void) {
  int setting = __atomic_load_n(&abortSetting, __ATOMIC_RELAXED);
  if (setting < 0) {
    const char *value = getenv("WEAVEC_RT_ABORT");
    setting = value != NULL && strcmp(value, "1") == 0;
    __atomic_store_n(&abortSetting, setting, __ATOMIC_RELAXED);
  }
  return setting;
}

/* WEAVEC_RT_REPORT_LOG=<path>: every report is also appended to that file,
 * one write per line, for a test harness that keeps the output of a passing
 * test to itself. */
static void logReport(const char *text, size_t size) {
  const char *path;
  int fd;
  /* A set-user-ID or set-group-ID program does not write where its
   * environment says. */
#if defined(__APPLE__)
  if (issetugid())
    return;
#else
  if (getauxval(AT_SECURE) != 0)
    return;
#endif
  path = getenv("WEAVEC_RT_REPORT_LOG");
  if (path == NULL || *path == 0)
    return;
  fd = open(path, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            0644);
  if (fd < 0)
    return;
  {
    const long written = (long)write(fd, text, size);
    (void)written;
  }
  (void)close(fd);
}

void __weavec_rt_report(const char *check, const char *file, unsigned line,
                        unsigned column) {
  const int abortNow = shouldAbort();
  char text[1024];
  int size;
  if (!abortNow && !firstReport(siteHash(check, file, line, column)))
    return;
  size = snprintf(text, sizeof text,
                  "weavec: runtime check failed: %s at %s:%u:%u\n",
                  check ? check : "?", file ? file : "<unknown>", line, column);
  if (size < 0)
    size = 0;
  if ((size_t)size >= sizeof text) {
    /* A path too long for the line: keep the line a line. */
    size = (int)sizeof text - 1;
    text[size - 1] = '\n';
  }
  fputs(text, stderr);
  fflush(stderr);
  logReport(text, (size_t)size);
  if (abortNow)
    abort();
}

/* Counted without synchronisation: a diagnostic aid, not an account. */
unsigned long long weavecRtStats[WeavecRtStatCount];

__attribute__((destructor)) static void printStats(void) {
  static const char *const Names[WeavecRtStatCount] = {
      "allocations",      "releases",      "recycled slots",
      "huge blocks",      "lookups",       "heap lookups",
      "stack lookups",    "global lookups", "untracked lookups",
      "range requests",   "ranges kept",   "stack objects entered"};
  const char *value = getenv("WEAVEC_RT_STATS");
  int i;
  if (value == NULL || strcmp(value, "1") != 0)
    return;
  for (i = 0; i < WeavecRtStatCount; ++i)
    fprintf(stderr, "weavec: runtime: %llu %s\n", weavecRtStats[i], Names[i]);
}

static char *appendText(char *at, const char *end, const char *text) {
  while (*text != 0 && at < end)
    *at++ = *text++;
  return at;
}

void __weavec_rt_fatal(const char *what, const void *p, const char *why) {
  char line[256] = {0};
  const char *end = line + sizeof line - 1;
  char *at = line;
  uintptr_t value = (uintptr_t)p;
  char digits[2 * sizeof value];
  unsigned count = 0;
  at = appendText(at, end, "weavec: ");
  at = appendText(at, end, what);
  at = appendText(at, end, " of 0x");
  do {
    digits[count++] = "0123456789abcdef"[value & 15];
    value >>= 4;
  } while (value != 0);
  while (count != 0 && at < end)
    *at++ = digits[--count];
  at = appendText(at, end, ": ");
  at = appendText(at, end, why);
  *at++ = '\n';
  {
    const long written = (long)write(2, line, (size_t)(at - line));
    (void)written;
  }
  __builtin_trap();
}
