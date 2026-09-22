/*===- weavec_rt.c - The report-mode runtime ---------------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0030, section 10.7. Under -fweavec-checks=report a failed check calls
|* __weavec_rt_report instead of trapping, and the program goes on, with no
|* guarantee. The runtime prints
|*
|*   weavec: runtime check failed: <template> at <file>:<line>:<column>
|*
|* to stderr once per site, where <template> is nonnull, index, span, len,
|* disjoint, assert or violation. With WEAVEC_RT_ABORT=1 in the environment
|* it prints the line and aborts. The case runner and gate G11 attribute
|* traps to lines and templates through this output (section 17.4).
|*
|* Installed as lib/weavec/libweavec_rt.a; weavec-cc links it when the link
|* is given -fweavec-checks=report.
|*
\*===----------------------------------------------------------------------===*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void __weavec_rt_report(const char *check, const char *file, unsigned line,
                        unsigned column);

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

void __weavec_rt_report(const char *check, const char *file, unsigned line,
                        unsigned column) {
  const int abortNow = shouldAbort();
  if (!abortNow && !firstReport(siteHash(check, file, line, column)))
    return;
  fprintf(stderr, "weavec: runtime check failed: %s at %s:%u:%u\n",
          check ? check : "?", file ? file : "<unknown>", line, column);
  fflush(stderr);
  if (abortNow)
    abort();
}
