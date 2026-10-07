/*===- weavec_report.c - Failures: reports and fatal errors -------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, section 5.3. A failed guard prints
|*
|*   weavec: <kind> at <file>:<line>:<column>: <read|write> of <n> bytes at <a>
|*
|* to stderr (a second line places a heap address in its object) and traps;
|* under -fweavec-checks=report it prints the line once per site and the
|* program goes on, with no guarantee. With WEAVEC_RT_ABORT=1 a report traps
|* too. The case runner and the corpus gate attribute traps to lines and
|* kinds through this output.
|*
|* __weavec_rt_fatal is how the allocator stops an invalid release: it
|* prints
|*
|*   weavec: <what> of <pointer>: <why>
|*
|* and traps. It uses no allocation and no stdio.
|*
|* __weavec_rt_trapping (RFC 0033, section 6.1) runs before every trap of a
|* failure: a trap the program has blocked or catches would otherwise repeat
|* forever on Darwin instead of ending the program.
|*
\*===----------------------------------------------------------------------===*/

#include "weavec_rt.h"

#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if !defined(__APPLE__)
#include <sys/auxv.h>
#endif

/* The sites already reported, as 64-bit hashes of (kind, file, line,
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

/* The settings below are read in a constructor, never while reporting: a
 * check can fail in a function the C library calls with its environment
 * lock held, and `getenv` takes it again. */

/* WEAVEC_RT_ABORT=1. */
static int abortSetting;

/* WEAVEC_RT_REPORT_LOG=<path>: every report is appended to that file, one
 * write per line, for a test harness that keeps the output of a passing test
 * to itself, and not printed (RFC 0033, section 6.3: a test that captures
 * standard error sees only the program's own output). */
static char reportLog[1024];

__attribute__((constructor)) static void readReportSettings(void) {
  const char *value = getenv("WEAVEC_RT_ABORT");
  const char *path;
  abortSetting = value != NULL && strcmp(value, "1") == 0;
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
  if (path != NULL && strlen(path) < sizeof reportLog)
    memcpy(reportLog, path, strlen(path) + 1);
}

static int shouldAbort(void) { return abortSetting; }

/* Appends a report to the log; returns whether it was written there. */
static int logReport(const char *text, size_t size) {
  int fd;
  if (reportLog[0] == 0)
    return 0;
  fd = open(reportLog, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
            0644);
  if (fd < 0)
    return 0;
  {
    const long written = (long)write(fd, text, size);
    (void)written;
  }
  (void)close(fd);
  return 1;
}

void __weavec_rt_trapping(void) {
  static const int Traps[2] = {SIGTRAP, SIGILL};
  sigset_t traps;
  int i;
  sigemptyset(&traps);
  for (i = 0; i < 2; ++i) {
    struct sigaction action;
    sigaddset(&traps, Traps[i]);
    /* A handler the program installed (a crash reporter) still runs; an
     * ignored trap would repeat forever. */
    if (sigaction(Traps[i], NULL, &action) == 0 &&
        (action.sa_flags & SA_SIGINFO) == 0 && action.sa_handler == SIG_IGN) {
      memset(&action, 0, sizeof action);
      action.sa_handler = SIG_DFL;
      (void)sigaction(Traps[i], &action, NULL);
    }
  }
  (void)pthread_sigmask(SIG_UNBLOCK, &traps, NULL);
}

/* Prints a report line (or appends it to the log). */
static void emit(const char *text, int size) {
  if (size < 0)
    return;
  if (!logReport(text, (size_t)size)) {
    const long written = (long)write(2, text, (size_t)size);
    (void)written;
  }
}

/* A line being written. The runtime formats its own numbers: a program may
 * define snprintf (an embedded printf without %ll). */
struct Line {
  char *at;
  char *end;
};

static void put(struct Line *line, const char *text) {
  while (*text != 0 && line->at < line->end)
    *line->at++ = *text++;
}

static void putUnsigned(struct Line *line, unsigned long long value,
                        unsigned base) {
  char digits[24];
  unsigned count = 0;
  do {
    digits[count++] = "0123456789abcdef"[value % base];
    value /= base;
  } while (value != 0);
  while (count != 0 && line->at < line->end)
    *line->at++ = digits[--count];
}

static void putHex(struct Line *line, unsigned long long value) {
  put(line, "0x");
  putUnsigned(line, value, 16);
}

/* Ends the line with a newline, which always fits, and returns its size. */
static int finish(struct Line *line, char *text) {
  *line->at++ = '\n';
  return (int)(line->at - text);
}

/* Where a heap address lies relative to its object, for the second line;
 * its size, 0 when there is nothing to say. */
static int describeHeap(uintptr_t address, char *text, size_t room) {
  const struct weavecRtHeapObject object = weavecRtHeapObjectAt(address);
  struct Line line = {text, text + room - 1};
  if (!object.found)
    return 0;
  put(&line, "weavec: ");
  putHex(&line, address);
  if (!object.live) {
    put(&line, " is inside a released heap block at ");
    putHex(&line, object.base);
  } else if (address >= object.base + object.size) {
    put(&line, " is ");
    putUnsigned(&line, address - object.base - object.size, 10);
    put(&line, " bytes after the ");
    putUnsigned(&line, object.size, 10);
    put(&line, "-byte heap object at ");
    putHex(&line, object.base);
  } else {
    return 0;
  }
  return finish(&line, text);
}

void weavecRtFail(const char *kind, uintptr_t address, uint64_t width,
                  const struct __weavec_rt_site *site) {
  const unsigned flags = site != NULL ? site->flags : 0;
  const char *file = site != NULL && site->file != NULL ? site->file : NULL;
  const unsigned line = site != NULL ? site->line : 0;
  const unsigned column = site != NULL ? site->column : 0;
  const int report = (flags & WeavecRtSiteReport) != 0 && !shouldAbort();
  char text[1024];
  struct Line out = {text, text + sizeof text - 1};
  int size;
  if (report && !firstReport(siteHash(kind, file, line, column)))
    return;
  put(&out, "weavec: ");
  if ((flags & WeavecRtSiteProven) != 0)
    put(&out, "weavec.proven: ");
  put(&out, kind);
  put(&out, " at ");
  if (file != NULL) {
    put(&out, file);
    put(&out, ":");
    putUnsigned(&out, line, 10);
    put(&out, ":");
    putUnsigned(&out, column, 10);
  } else {
    put(&out, "<unknown>");
  }
  put(&out, ": ");
  if (strcmp(kind, "index-out-of-bounds") == 0) {
    put(&out, "index ");
    if ((long long)address < 0) {
      put(&out, "-");
      putUnsigned(&out, 0 - (unsigned long long)address, 10);
    } else {
      putUnsigned(&out, address, 10);
    }
  } else if (width == 0) {
    put(&out, "access at ");
    putHex(&out, address);
  } else {
    put(&out, (flags & WeavecRtSiteWrite) != 0 ? "write" : "read");
    put(&out, " of ");
    putUnsigned(&out, width, 10);
    put(&out, " bytes at ");
    putHex(&out, address);
  }
  emit(text, finish(&out, text));
  if (strncmp(kind, "heap-", 5) == 0) {
    size = describeHeap(address, text, sizeof text);
    if (size > 0)
      emit(text, size);
  }
  if (report)
    return;
  __weavec_rt_trapping();
  __builtin_trap();
}

/* Counted without synchronisation: a diagnostic aid, not an account. */
unsigned long long weavecRtStats[WeavecRtStatCount];

__attribute__((destructor)) static void printStats(void) {
  static const char *const Names[WeavecRtStatCount] = {
      "allocations", "releases",      "recycled slots",   "huge blocks",
      "slow guards", "range guards",  "string guards",    "stack unpoisons"};
  const char *value = getenv("WEAVEC_RT_STATS");
  int i;
  /* (The owner's counters count every image's work.) */
  if (weavecRtForward() != 0 || value == NULL || strcmp(value, "1") != 0)
    return;
  for (i = 0; i < WeavecRtStatCount; ++i) {
    char text[128];
    struct Line line = {text, text + sizeof text - 1};
    put(&line, "weavec: runtime: ");
    putUnsigned(&line, weavecRtStats[i], 10);
    put(&line, " ");
    put(&line, Names[i]);
    {
      const long written = (long)write(2, text, (size_t)finish(&line, text));
      (void)written;
    }
  }
}

void __weavec_rt_fatal(const char *what, const void *p, const char *why) {
  {
    const struct __weavec_rt_dispatch *owner = weavecRtForward();
    if (owner != 0) {
      owner->fatal(what, p, why);
      __builtin_unreachable();
    }
  }
  char text[256];
  struct Line line = {text, text + sizeof text - 1};
  put(&line, "weavec: ");
  put(&line, what);
  put(&line, " of ");
  putHex(&line, (uintptr_t)p);
  put(&line, ": ");
  put(&line, why);
  {
    const long written = (long)write(2, text, (size_t)finish(&line, text));
    (void)written;
  }
  __weavec_rt_trapping();
  __builtin_trap();
}
