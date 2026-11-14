/*===- weavec_report.c - Reports ----------------------------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035 section 5.3, RFC 0040 section 5. A failure prints
|*
|*   weavec: <kind> at <file>:<line>:<column>: <what>
|*       #0 <pc> in <function> <file>:<line>:<column>
|*       ...
|*   weavec: <the object the address lies in or near>
|*       #0 ... (a heap block's release and allocation)
|*   weavec: summary: <kind> at <location> in <function> (thread T<n>)
|*
|* to stderr (or WEAVEC_RT_REPORT_LOG), or one line of JSON under
|* WEAVEC_RT_REPORT_FORMAT=json (section 6), and traps; under
|* -fweavec-checks=report it prints a site once and the program goes on,
|* with no guarantee. With WEAVEC_RT_ABORT=1 a report traps too. A bad
|* release (section 5.5) and a failed check of the C++ library stop the
|* program in every mode. A report is formatted in a buffer of the
|* runtime's under a lock, and written at once.
|*
|* __weavec_rt_trapping (RFC 0033, section 6.1) runs before every trap of a
|* failure: a trap the program has blocked or catches would otherwise repeat
|* forever on Darwin instead of ending the program.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1 /* glibc's REG_ERR and REG_RIP */
#endif

#include "weavec_rt.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/ucontext.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#include <string.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <link.h>
#include <sys/auxv.h>
#endif

/*===-- Report mode's sites -------------------------------------------------===*/

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

/* True the first time a site is seen; safe under concurrent reports. */
static int firstReport(unsigned long long hash) {
  unsigned probe;
  hash = hash != 0 ? hash : 1;
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

/*===-- Settings ------------------------------------------------------------===*/

/* Read in a constructor, never while reporting: a check can fail in a
 * function the C library calls with its environment lock held, and
 * `getenv` takes it again. WEAVEC_RT_ABORT=1;
 * WEAVEC_RT_REPORT_FORMAT=json; WEAVEC_RT_REPORT_LOG=<path>: every report is
 * appended to that file and not printed (RFC 0033, section 6.3: a test that
 * captures standard error sees only the program's own output). */
static int abortSetting;
static int jsonSetting;
static char reportLog[1024];

__attribute__((constructor)) static void readReportSettings(void) {
  const char *value = getenv("WEAVEC_RT_ABORT");
  const char *path;
  abortSetting = value != NULL && strcmp(value, "1") == 0;
  value = getenv("WEAVEC_RT_REPORT_FORMAT");
  jsonSetting = value != NULL && strcmp(value, "json") == 0;
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

/* Writes a report to the log, or else to stderr, in one write. */
static void emit(const char *text, size_t size) {
  int fd = -1;
  long written;
  if (reportLog[0] != 0)
    fd = open(reportLog, O_WRONLY | O_APPEND | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
              0644);
  written = (long)write(fd >= 0 ? fd : 2, text, size);
  (void)written;
  if (fd >= 0)
    (void)close(fd);
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

static __attribute__((noreturn)) void trapNow(void) {
  __weavec_rt_trapping();
  __builtin_trap();
}

/*===-- Text ----------------------------------------------------------------===*/

/* Text being written. The runtime formats its own numbers: a program may
 * define snprintf (an embedded printf without %ll). */
struct Line {
  char *at;
  char *end;
};

static void put(struct Line *line, const char *text) {
  while (text != NULL && *text != 0 && line->at < line->end)
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

/* A string in JSON, quoted; null as `null`. */
static void putJson(struct Line *line, const char *text) {
  if (text == NULL) {
    put(line, "null");
    return;
  }
  put(line, "\"");
  for (; *text != 0 && line->at + 6 < line->end; ++text) {
    const unsigned char c = (unsigned char)*text;
    if (c == '"' || c == '\\') {
      *line->at++ = '\\';
      *line->at++ = (char)c;
    } else if (c < 0x20) {
      put(line, "\\u00");
      *line->at++ = "0123456789abcdef"[c >> 4];
      *line->at++ = "0123456789abcdef"[c & 15];
    } else {
      *line->at++ = (char)c;
    }
  }
  put(line, "\"");
}

/* `name` (`,"field":`) and the value, a number or a quoted address. */
static void putNumber(struct Line *line, const char *name,
                      unsigned long long value) {
  put(line, name);
  putUnsigned(line, value, 10);
}

static void putJsonHex(struct Line *line, const char *name,
                       unsigned long long value) {
  put(line, name);
  put(line, "\"");
  putHex(line, value);
  put(line, "\"");
}

/* A C++ name demangled by the C++ library's own __cxa_demangle, when the
 * process has one (RFC 0039 section 8). */
static void putFunction(struct Line *line, const char *name) {
  typedef char *Demangle(const char *, char *, size_t *, int *);
  static Demangle *demangle;
  static int looked;
  char *plain = NULL;
  int status = -1;
  if (name == NULL) {
    put(line, "?");
    return;
  }
  if (!looked) {
    demangle = (Demangle *)dlsym(RTLD_DEFAULT, "__cxa_demangle");
    looked = 1;
  }
  if (demangle != NULL && strncmp(name, "_Z", 2) == 0)
    plain = demangle(name, NULL, NULL, &status);
  put(line, plain != NULL && status == 0 ? plain : name);
  free(plain);
}

/*===-- The report being written --------------------------------------------===*/

struct Stack {
  unsigned count;
  unsigned thread;
  /* Where its frames are in `symbols`. */
  unsigned first;
  uintptr_t pc[WeavecRtReportFrames];
};

enum { NoMemory, HeapMemory, StackMemory, GlobalMemory, Destination };

/* One report's facts, from which its text or JSON is written. */
static struct Report {
  const char *kind;
  int proven;
  int writes;
  /* The failing access, or its release (`op` "free" and so on). */
  const char *op;
  uint64_t width;
  uintptr_t address;
  /* The location: the site's, or the stack's first frame's. */
  const char *file;
  unsigned line;
  unsigned column;
  /* A fault's instruction, which its first line names (RFC 0037 §6). */
  uintptr_t pc;
  char what[640];
  /* A line before the object's (a color mismatch's root). */
  char note[256];
  const char *message;
  struct Stack stack;
  /* The object paragraphs. */
  int memory;
  char object[1024];
  struct weavecRtHeapObject heap;
  struct Stack released;
  struct Stack allocated;
  /* A fortified call's destination [lo, hi). */
  uintptr_t lo;
  uintptr_t hi;
  /* A frame's function, local and locals; a global's name and location. */
  const struct __weavec_rt_frame *frame;
  uintptr_t frameBase;
  int local;
  const struct __weavec_rt_global *global;
} report;

static WeavecRtLock reportLock;
static int reportOwner = -1;
static struct weavecRtFrameInfo symbols[3 * WeavecRtReportFrames];
static char reportText[65536];

/* The thread `n`, with its name when it is the calling one's (the main
 * thread's is the program's). */
static void putThread(struct Line *line, unsigned n) {
  char name[64];
  putNumber(line, "T", n);
  if (n != 0 && n == weavecRtThread() &&
      pthread_getname_np(pthread_self(), name, sizeof name) == 0 &&
      name[0] != 0) {
    put(line, " '");
    put(line, name);
    put(line, "'");
  }
}

static const struct weavecRtSymbol *innermost(const struct Stack *stack) {
  return stack->count != 0 ? &symbols[stack->first].lines[0] : NULL;
}

/* A stack's frames: `    #<i> <pc> in <function> <file>:<line>:<column>`,
 * an inlined call's lines under one number. */
static void putStack(struct Line *line, const struct Stack *stack) {
  unsigned i;
  unsigned k;
  for (i = 0; i < stack->count; ++i) {
    const struct weavecRtFrameInfo *frame = &symbols[stack->first + i];
    for (k = 0; k < frame->count; ++k) {
      const struct weavecRtSymbol *symbol = &frame->lines[k];
      putNumber(line, "    #", i);
      put(line, " ");
      putHex(line, stack->pc[i]);
      put(line, " in ");
      putFunction(line, symbol->function);
      put(line, " ");
      if (symbol->file != NULL) {
        put(line, symbol->file);
        putNumber(line, ":", symbol->line);
        if (symbol->column != 0) {
          putNumber(line, ":", symbol->column);
        }
      } else {
        put(line, "(");
        put(line, frame->module != NULL ? frame->module : "?");
        put(line, "+");
        putHex(line, frame->offset);
        put(line, ")");
      }
      put(line, "\n");
    }
  }
}

static void putJsonStack(struct Line *line, const struct Stack *stack) {
  unsigned i;
  unsigned k;
  const char *comma = "";
  put(line, "[");
  /* (Frames that do not fit leave room to end the line as JSON.) */
  for (i = 0; i < stack->count && line->end - line->at > 8192; ++i) {
    const struct weavecRtFrameInfo *frame = &symbols[stack->first + i];
    for (k = 0; k < frame->count; ++k) {
      const struct weavecRtSymbol *symbol = &frame->lines[k];
      char name[512];
      struct Line function = {name, name + sizeof name - 1};
      putFunction(&function, symbol->function);
      *function.at = 0;
      put(line, comma);
      putJsonHex(line, "{\"pc\":", stack->pc[i]);
      put(line, ",\"module\":");
      putJson(line, frame->module);
      putJsonHex(line, ",\"offset\":", frame->offset);
      put(line, ",\"function\":");
      putJson(line, symbol->function != NULL ? name : NULL);
      put(line, ",\"file\":");
      putJson(line, symbol->file);
      putNumber(line, ",\"line\":", symbol->line);
      putNumber(line, ",\"column\":", symbol->column);
      put(line, k + 1 < frame->count ? ",\"inlined\":true}"
                                     : ",\"inlined\":false}");
      comma = ",";
    }
  }
  put(line, "]");
}

/* A heap block's stack from the store, or none. */
static void loadStack(struct Stack *stack, uint32_t id) {
  stack->count = weavecRtStackGet(id, stack->pc, WeavecRtHeapFrames,
                                  &stack->thread);
}

/* `, released by thread T<n>:` (its stack follows), or that it was not
 * recorded. */
static void putWhose(struct Line *line, const char *what,
                     const struct Stack *stack) {
  if (stack->count != 0) {
    put(line, ", ");
    put(line, what);
    put(line, " by thread ");
    putThread(line, stack->thread);
    put(line, ":");
    return;
  }
  put(line, "; it was ");
  put(line, what);
  put(line, weavecRtHeapStacks ? " where no stack was recorded"
                               : " (not recorded: WEAVEC_RT_HEAP_STACKS=0)");
}

/*===-- Objects (sections 5.2 to 5.4) ---------------------------------------===*/

/* Where `address` lies relative to [lo, hi): `<n> bytes after the`, ... */
static void putPlace(struct Line *line, uintptr_t address, uintptr_t lo,
                     uintptr_t hi) {
  if (address >= hi && hi > lo) {
    putUnsigned(line, address - hi, 10);
    put(line, " bytes after ");
  } else if (address < lo) {
    putUnsigned(line, lo - address, 10);
    put(line, " bytes before ");
  } else {
    putUnsigned(line, address - lo, 10);
    put(line, " bytes inside ");
  }
}

/* The heap paragraph of an address the allocator knows. */
static int describeHeap(uintptr_t address) {
  struct weavecRtHeapObject *object = &report.heap;
  struct Line line = {report.object, report.object + sizeof report.object - 1};
  *object = weavecRtHeapObjectAt(address);
  if (!object->found)
    return 0;
  report.memory = HeapMemory;
  loadStack(&report.allocated, object->allocated);
  if (!object->live)
    loadStack(&report.released, object->released);
  putHex(&line, address);
  put(&line, " is ");
  /* RFC 0039 section 5: past the elements, which end at the first container
   * granule (or inside the partial one before it). */
  if (object->live && address < object->base + object->size &&
      (*weavecRtShadowOf(address) == WeavecRtShadowContainer ||
       *weavecRtShadowOf((address | 15) + 1) == WeavecRtShadowContainer)) {
    uintptr_t end = object->base;
    while (*weavecRtShadowOf(end) != WeavecRtShadowContainer)
      end += 16;
    if (end > object->base && !weavecRtShadowFull(*weavecRtShadowOf(end - 16)))
      end -= 16 - weavecRtShadowBytes(*weavecRtShadowOf(end - 16));
    putUnsigned(&line, address >= end ? address - end : 0, 10);
    put(&line, " bytes past the elements of a container in the ");
  } else if (address == object->base && report.op != NULL &&
             report.width == 0) {
    put(&line, "the "); /* (a release) */
  } else {
    putPlace(&line, address, object->base, object->base + object->size);
    put(&line, "the ");
  }
  putUnsigned(&line, object->size, 10);
  put(&line, object->live ? "-byte heap object at " : "-byte heap block at ");
  putHex(&line, object->base);
  if (object->live)
    putWhose(&line, "allocated", &report.allocated);
  else
    putWhose(&line, "released", &report.released);
  *line.at = 0;
  return 1;
}

/* Whether the page holding `at` is mapped. */
static int mapped(uintptr_t at) {
  const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
  unsigned char resident;
  return mincore((void *)(at & ~(page - 1)), 1, (void *)&resident) == 0;
}

/* RFC 0040 section 5.3: the frame that holds a stack address. Below the
 * address, at the first 32-byte boundary whose shadow is a left redzone or a
 * returned frame's, the two words a frame stores name its descriptor. */
static int describeFrame(uintptr_t address) {
  const uintptr_t bottom = weavecRtStackBottom();
  const uintptr_t top = weavecRtStackTop();
  const int own = top != 0 && address >= bottom && address < top;
  uintptr_t at = address & ~(uintptr_t)31;
  unsigned steps;
  const struct __weavec_rt_frame *frame = NULL;
  struct Line line = {report.object, report.object + sizeof report.object - 1};
  uint32_t i;
  int nearest = -1;
  uint64_t distance = ~(uint64_t)0;
  if (!weavecRtShadowReady() || !weavecRtInWindow(address))
    return 0;
  for (steps = 0; steps < 65536 && (!own || at >= bottom); ++steps, at -= 32) {
    const unsigned char value = *weavecRtShadowOf(at);
    const uintptr_t *words = (const uintptr_t *)at;
    if (value != WeavecRtShadowStackLeft && value != WeavecRtShadowReturned)
      continue;
    /* (Another thread's stack may have been unmapped since its shadow was
     * written.) */
    if (!own && !mapped(at))
      return 0;
    if (words[0] != WeavecRtFrameMagic)
      continue;
    frame = (const struct __weavec_rt_frame *)words[1];
    if (!weavecRtFrameKnown(frame) || address >= at + frame->reserved + 64)
      return 0;
    break;
  }
  if (frame == NULL)
    return 0;
  report.memory = StackMemory;
  report.frame = frame;
  report.frameBase = at;
  /* The local the address lies in, or nearest. */
  for (i = 0; i < frame->objects; ++i) {
    const uintptr_t lo = at + frame->object[i].offset;
    const uintptr_t hi = lo + frame->object[i].size;
    const uint64_t d = address < lo ? lo - address
                       : address >= hi ? address - hi + 1
                                       : 0;
    if (d < distance)
      distance = d, nearest = (int)i;
  }
  report.local = nearest;
  putHex(&line, address);
  put(&line, " is ");
  if (nearest >= 0) {
    const struct __weavec_rt_frame_object *local = &frame->object[nearest];
    const uintptr_t lo = at + local->offset;
    putPlace(&line, address, lo, lo + local->size);
    put(&line, "'");
    put(&line, (const char *)local + local->name);
    put(&line, "' (");
    putUnsigned(&line, local->size, 10);
    put(&line, " bytes at offset ");
    putUnsigned(&line, local->offset, 10);
    put(&line, ")");
    if (strcmp(report.kind, "stack-use-after-scope") == 0)
      put(&line, ", whose scope has ended,");
  } else {
    put(&line, "in a frame");
  }
  put(&line, strcmp(report.kind, "stack-use-after-return") == 0
                 ? " in a frame of "
                 : " in the frame of ");
  put(&line, "\x01"); /* (the function's name, demangled when written) */
  if (strcmp(report.kind, "stack-use-after-return") == 0)
    put(&line, " that returned");
  if (own) {
    put(&line, " (thread ");
    putThread(&line, weavecRtThread());
    put(&line, ")");
  }
  put(&line, frame->objects != 0 ? ", whose locals are:" : "");
  *line.at = 0;
  return 1;
}

/* RFC 0040 section 5.4. */
static int describeGlobal(uintptr_t address) {
  const struct __weavec_rt_global *global = weavecRtGlobalAt(address);
  struct Line line = {report.object, report.object + sizeof report.object - 1};
  if (global == NULL)
    return 0;
  report.memory = GlobalMemory;
  report.global = global;
  putHex(&line, address);
  put(&line, " is ");
  putPlace(&line, address, global->address, global->address + global->size);
  put(&line, "the ");
  putUnsigned(&line, global->size, 10);
  put(&line, "-byte global '");
  putFunction(&line, global->name != 0 ? (const char *)global + global->name
                                        : "?");
  put(&line, "'");
  if (global->location != 0) {
    put(&line, " defined at ");
    put(&line, (const char *)global + global->location);
  }
  *line.at = 0;
  return 1;
}

/* The object paragraph of `address`, by what memory it is. */
static void describe(uintptr_t address) {
  if (strncmp(report.kind, "stack-", 6) == 0 &&
      strcmp(report.kind, "dynamic-stack-buffer-overflow") != 0) {
    /* (A frame that returned is overwritten by the report's own frames
     * unless it was deep below.) */
    if (!describeFrame(address) &&
        strcmp(report.kind, "stack-use-after-return") == 0) {
      struct Line line = {report.note, report.note + sizeof report.note - 1};
      putHex(&line, address);
      put(&line, " is in a frame that returned");
      *line.at = 0;
    }
    return;
  }
  if (strncmp(report.kind, "global-", 7) == 0) {
    (void)describeGlobal(address);
    return;
  }
  if (!describeHeap(address) && !describeGlobal(address))
    (void)describeFrame(address);
}

/*===-- Writing -------------------------------------------------------------===*/

static void putLocation(struct Line *line, int summary) {
  if (report.pc != 0 && !summary) {
    put(line, "pc ");
    putHex(line, report.pc);
    return;
  }
  if (report.file == NULL) {
    put(line, "<unknown>");
    return;
  }
  put(line, report.file);
  putNumber(line, ":", report.line);
  if (report.column != 0) {
    putNumber(line, ":", report.column);
  }
}

static void putFrameLocals(struct Line *line) {
  const struct __weavec_rt_frame *frame = report.frame;
  uint32_t i;
  for (i = 0; i < frame->objects && i < 32; ++i) {
    const struct __weavec_rt_frame_object *local = &frame->object[i];
    putNumber(line, "    [", local->offset);
    putNumber(line, ", ", (uint64_t)local->offset + local->size);
    put(line, ") '");
    put(line, (const char *)local + local->name);
    put(line, "'\n");
  }
}

static void writeText(struct Line *line) {
  const struct weavecRtSymbol *first = innermost(&report.stack);
  const char *at;
  put(line, "weavec: ");
  if (report.proven)
    put(line, "weavec.proven: ");
  put(line, report.kind);
  put(line, " at ");
  putLocation(line, 0);
  put(line, ": ");
  put(line, report.what);
  put(line, "\n");
  putStack(line, &report.stack);
  if (report.note[0] != 0) {
    put(line, "weavec: ");
    put(line, report.note);
    put(line, "\n");
  }
  if (report.memory != NoMemory) {
    put(line, "weavec: ");
    for (at = report.object; *at != 0; ++at)
      if (*at == '\x01')
        putFunction(line, (const char *)report.frame + report.frame->function);
      else if (line->at < line->end)
        *line->at++ = *at;
    put(line, "\n");
  }
  if (report.memory == HeapMemory) {
    putStack(line, report.heap.live ? &report.allocated : &report.released);
    if (!report.heap.live) {
      put(line, "weavec: the block was allocated");
      if (report.allocated.count != 0) {
        put(line, " by thread ");
        putThread(line, report.allocated.thread);
        put(line, ":\n");
        putStack(line, &report.allocated);
      } else {
        put(line, weavecRtHeapStacks
                      ? " where no stack was recorded\n"
                      : " (not recorded: WEAVEC_RT_HEAP_STACKS=0)\n");
      }
    }
    /* RFC 0045 section 9: a fast program reuses blocks at once. */
    if (weavecRtFastProgram)
      put(line, "weavec: note: built for speed; rebuild with "
                "-fweavec-safe-checks=full for allocation stacks and exact "
                "use-after-free reports\n");
  } else if (report.memory == StackMemory) {
    putFrameLocals(line);
  }
  put(line, "weavec: summary: ");
  put(line, report.kind);
  put(line, " at ");
  putLocation(line, 1);
  if (first != NULL && first->function != NULL) {
    put(line, " in ");
    putFunction(line, first->function);
  }
  put(line, " (thread ");
  putThread(line, weavecRtThread());
  put(line, ")\n");
}

static void writeJson(struct Line *line) {
  putNumber(line, "{\"weavec\":", WEAVEC_RT_ABI);
  put(line, ",\"kind\":");
  putJson(line, report.kind);
  put(line, report.proven ? ",\"proven\":true" : ",\"proven\":false");
  put(line, ",\"site\":{\"file\":");
  putJson(line, report.file);
  putNumber(line, ",\"line\":", report.line);
  putNumber(line, ",\"column\":", report.column);
  put(line, "},\"what\":");
  putJson(line, report.what);
  if (report.op != NULL) {
    put(line, ",\"access\":{\"op\":");
    putJson(line, report.op);
    putNumber(line, ",\"width\":", report.width);
    putJsonHex(line, ",\"address\":", report.address);
    put(line, "}");
  }
  if (report.message != NULL) {
    put(line, ",\"message\":");
    putJson(line, report.message);
  }
  putNumber(line, ",\"thread\":{\"id\":", weavecRtThread());
  put(line, "},\"stack\":");
  putJsonStack(line, &report.stack);
  if (report.memory == HeapMemory) {
    putJsonHex(line, ",\"object\":{\"memory\":\"heap\",\"base\":", report.heap.base);
    putNumber(line, ",\"size\":", report.heap.size);
    put(line, report.heap.live ? ",\"state\":\"live\"" : ",\"state\":\"released\"");
    put(line, ",\"released\":");
    if (!report.heap.live && report.released.count != 0)
      putJsonStack(line, &report.released);
    else
      put(line, "null");
    put(line, ",\"allocated\":");
    if (report.allocated.count != 0)
      putJsonStack(line, &report.allocated);
    else
      put(line, "null");
    put(line, "}");
  } else if (report.memory == StackMemory) {
    const struct __weavec_rt_frame *frame = report.frame;
    char name[512];
    struct Line function = {name, name + sizeof name - 1};
    uint32_t i;
    putFunction(&function, (const char *)frame + frame->function);
    *function.at = 0;
    put(line, ",\"object\":{\"memory\":\"stack\",\"function\":");
    putJson(line, name);
    put(line, ",\"name\":");
    putJson(line, report.local >= 0
                      ? (const char *)&frame->object[report.local] +
                            frame->object[report.local].name
                      : NULL);
    put(line, ",\"locals\":[");
    for (i = 0; i < frame->objects; ++i) {
      put(line, i != 0 ? ",{\"name\":" : "{\"name\":");
      putJson(line, (const char *)&frame->object[i] + frame->object[i].name);
      putNumber(line, ",\"offset\":", frame->object[i].offset);
      putNumber(line, ",\"size\":", frame->object[i].size);
      put(line, "}");
    }
    put(line, "]}");
  } else if (report.memory == GlobalMemory) {
    const struct __weavec_rt_global *global = report.global;
    put(line, ",\"object\":{\"memory\":\"global\",\"name\":");
    putJson(line, global->name != 0 ? (const char *)global + global->name
                                    : NULL);
    put(line, ",\"location\":");
    putJson(line, global->location != 0
                      ? (const char *)global + global->location
                      : NULL);
    putJsonHex(line, ",\"base\":", global->address);
    putNumber(line, ",\"size\":", global->size);
    put(line, "}");
  } else if (report.memory == Destination) {
    putJsonHex(line, ",\"object\":{\"memory\":\"destination\",\"base\":", report.lo);
    putNumber(line, ",\"size\":", report.hi - report.lo);
    put(line, "}");
  }
  put(line, "}\n");
}

/* Starts a report: false when one is being written by this thread (a
 * failure inside a report). */
static int begin(const char *kind, const struct __weavec_rt_site *site) {
  const int self = (int)weavecRtThread();
  if (__atomic_load_n(&reportOwner, __ATOMIC_RELAXED) == self)
    return 0;
  weavecRtLock(&reportLock);
  __atomic_store_n(&reportOwner, self, __ATOMIC_RELAXED);
  memset(&report, 0, sizeof report);
  report.kind = kind;
  report.local = -1;
  if (site != NULL) {
    /* RFC 0045 section 10: a believed access through a null pointer (a
     * zeroed object's) faults in the fast build too; it is no hole. */
    report.proven = (site->flags & WeavecRtSiteProven) != 0 &&
                    !((site->flags & WeavecRtSiteBelieved) != 0 &&
                      strcmp(kind, "null-dereference") == 0);
    report.writes = (site->flags & WeavecRtSiteWrite) != 0;
    report.file = weavecRtSiteFile(site);
    report.line = site->line;
    report.column = site->column;
  }
  return 1;
}

/* Symbolizes every stack of the report, writes it and ends it. */
static void finish(void) {
  uintptr_t pcs[3 * WeavecRtReportFrames];
  unsigned count = 0;
  struct Stack *const stacks[3] = {&report.stack, &report.released,
                                   &report.allocated};
  unsigned i;
  struct Line line = {reportText, reportText + sizeof reportText - 2};
  for (i = 0; i < 3; ++i) {
    stacks[i]->first = count;
    memcpy(&pcs[count], stacks[i]->pc, stacks[i]->count * sizeof(uintptr_t));
    count += stacks[i]->count;
  }
  weavecRtSymbolize(pcs, count, symbols);
  /* A location the site does not give: the first frame's. */
  if (report.file == NULL && innermost(&report.stack) != NULL &&
      innermost(&report.stack)->file != NULL) {
    report.file = innermost(&report.stack)->file;
    report.line = innermost(&report.stack)->line;
    report.column = innermost(&report.stack)->column;
  }
  if (jsonSetting)
    writeJson(&line);
  else
    writeText(&line);
  /* (A report cut at the buffer's end still ends its line.) */
  if (line.at == line.end)
    *line.at++ = '\n';
  emit(reportText, (size_t)(line.at - reportText));
  __atomic_store_n(&reportOwner, -1, __ATOMIC_RELAXED);
  weavecRtUnlock(&reportLock);
}

/* The access's stack: the program's frames from its call into the
 * runtime. */
static __attribute__((noinline)) void captureAccess(void) {
  report.stack.count =
      weavecRtUnwind(report.stack.pc, WeavecRtReportFrames,
                     (uintptr_t)__builtin_frame_address(0), 0);
  report.stack.thread = weavecRtThread();
}

/* The first line's `<what>` of an access. */
static void accessWhat(uintptr_t address, uint64_t width) {
  struct Line line = {report.what, report.what + sizeof report.what - 1};
  report.address = address;
  report.width = width;
  if (strcmp(report.kind, "index-out-of-bounds") == 0) {
    put(&line, "index ");
    if ((long long)address < 0) {
      put(&line, "-");
      putUnsigned(&line, 0 - (unsigned long long)address, 10);
    } else {
      putUnsigned(&line, address, 10);
    }
    report.op = NULL;
  } else if (strcmp(report.kind, "bounds-conversion") == 0) {
    /* RFC 0042 section 6: what the pointer's destination needs. */
    put(&line, "needs ");
    putUnsigned(&line, width, 10);
    put(&line, " bytes at ");
    putHex(&line, address);
    report.op = "convert";
  } else if (width == 0) {
    put(&line, "access at ");
    putHex(&line, address);
    report.op = "access";
  } else {
    report.op = report.writes ? "write" : "read";
    put(&line, report.op);
    put(&line, " of ");
    putUnsigned(&line, width, 10);
    put(&line, " bytes at ");
    putHex(&line, address);
  }
  *line.at = 0;
}

/* Whether report mode reported the site already (or should not stop). */
static int reportsOnce(const struct __weavec_rt_site *site) {
  return site != NULL && (site->flags & WeavecRtSiteReport) != 0 &&
         !abortSetting;
}

static int seenSite(const char *kind, const struct __weavec_rt_site *site) {
  unsigned long long hash = hashText(14695981039346656037ULL, kind);
  hash = hashText(hash, weavecRtSiteFile(site));
  hash = (hash ^ site->line) * 1099511628211ULL;
  hash = (hash ^ site->column) * 1099511628211ULL;
  return !firstReport(hash);
}

static void stop(const struct __weavec_rt_site *site) {
  if (!reportsOnce(site))
    trapNow();
}

/*===-- Failures ------------------------------------------------------------===*/

/* RFC 0039 section 6: the C++ library's code after a failed check is
 * unreachable, so the report stops the program in every mode. */
void weavecRtFailMessage(const char *kind, const char *message,
                         const struct __weavec_rt_site *site) {
  if (begin(kind, site)) {
    struct Line line = {report.what, report.what + sizeof report.what - 1};
    put(&line, message);
    *line.at = 0;
    report.message = message;
    captureAccess();
    finish();
  }
  trapNow();
}

/* A report whose object paragraph is `address`'s, or the one
 * `paragraph` writes. */
static void fail(const char *kind, uintptr_t address, uint64_t width,
                 const struct __weavec_rt_site *site, int object) {
  if (reportsOnce(site) && seenSite(kind, site))
    return;
  if (begin(kind, site)) {
    accessWhat(address, width);
    captureAccess();
    if (object)
      describe(address);
    finish();
  }
}

void weavecRtFail(const char *kind, uintptr_t address, uint64_t width,
                  const struct __weavec_rt_site *site) {
  fail(kind, address, width, site,
       strcmp(kind, "null-dereference") != 0 &&
           strcmp(kind, "index-out-of-bounds") != 0 &&
           strcmp(kind, "overlapping-copy") != 0);
  stop(site);
}

/* RFC 0043 section 7: the first line says what was loaded, not an access;
 * the paragraph is the released block's. */
void weavecRtFailReference(uintptr_t address,
                           const struct __weavec_rt_site *site) {
  static const char kind[] = "heap-use-after-free";
  if (reportsOnce(site) && seenSite(kind, site))
    return;
  if (begin(kind, site)) {
    struct Line line = {report.what, report.what + sizeof report.what - 1};
    report.address = address;
    report.width = 0;
    report.op = "load";
    put(&line, "reference to ");
    putHex(&line, address);
    put(&line, " loaded from a field");
    *line.at = 0;
    captureAccess();
    describe(address);
    finish();
  }
  stop(site);
}

/* RFC 0036 section 6: the destination [lo, hi) of a fortified call;
 * RFC 0041 section 4: a safe function's pointer's bounds. */
void weavecRtFailObject(const char *kind, const char *what, uintptr_t address,
                        uint64_t width, uintptr_t lo, uintptr_t hi,
                        const struct __weavec_rt_site *site) {
  if (reportsOnce(site) && seenSite(kind, site))
    return;
  if (begin(kind, site)) {
    struct Line line = {report.object, report.object + sizeof report.object - 1};
    accessWhat(address, width);
    captureAccess();
    report.memory = Destination;
    report.lo = lo;
    report.hi = hi;
    putHex(&line, address);
    put(&line, " is ");
    putPlace(&line, address, lo, hi);
    put(&line, "the ");
    putUnsigned(&line, hi - lo, 10);
    put(&line, "-byte ");
    put(&line, what);
    put(&line, " at ");
    putHex(&line, lo);
    *line.at = 0;
    finish();
  }
  stop(site);
}

/* RFC 0037 section 8.3: a color mismatch names the root the access was
 * computed from, and the object the access landed in. */
void weavecRtFailJump(const char *kind, uintptr_t address, uint64_t width,
                      uintptr_t root, const struct __weavec_rt_site *site) {
  if (reportsOnce(site) && seenSite(kind, site))
    return;
  if (begin(kind, site)) {
    struct Line line = {report.note, report.note + sizeof report.note - 1};
    accessWhat(address, width);
    putHex(&line, address);
    put(&line, " is ");
    putUnsigned(&line, address >= root ? address - root : root - address, 10);
    put(&line, address >= root ? " bytes after its base "
                               : " bytes before its base ");
    putHex(&line, root);
    put(&line, ", in another object");
    *line.at = 0;
    captureAccess();
    describe(address);
    finish();
  }
  stop(site);
}

static const char *formName(unsigned form) {
  return form == WeavecRtFormNew        ? "operator new"
         : form == WeavecRtFormNewArray ? "operator new[]"
                                        : "malloc";
}

const char *weavecRtReleaseName(unsigned form) {
  return form == WeavecRtFormNew        ? "operator delete"
         : form == WeavecRtFormNewArray ? "operator delete[]"
                                        : "free";
}

/* RFC 0040 section 5.5. */
void weavecRtFailRelease(const struct weavecRtRelease *bad) {
  static const char *const Kinds[3] = {"double-free", "invalid-free",
                                       "alloc-release-mismatch"};
  if (begin(Kinds[bad->kind], bad->site)) {
    struct Line line = {report.what, report.what + sizeof report.what - 1};
    const uintptr_t address = (uintptr_t)bad->p;
    report.op = bad->by;
    report.address = address;
    captureAccess();
    (void)describeHeap(address);
    put(&line, bad->by);
    put(&line, " of ");
    putHex(&line, address);
    if (bad->kind == WeavecRtInvalidFree) {
      put(&line, ": ");
      if (bad->why != NULL) {
        put(&line, bad->why);
      } else {
        put(&line, "inside the ");
        putUnsigned(&line, report.heap.size, 10);
        put(&line, "-byte heap block at ");
        putHex(&line, report.heap.base);
      }
      if (report.memory == NoMemory && !describeGlobal(address))
        (void)describeFrame(address);
    } else if (bad->kind == WeavecRtMismatch) {
      if (bad->size == SIZE_MAX) {
        put(&line, ": allocated by ");
        put(&line, formName(bad->allocatedBy));
      } else {
        put(&line, ": size ");
        putUnsigned(&line, bad->size, 10);
        put(&line, " for a ");
        putUnsigned(&line, report.heap.size, 10);
        put(&line, "-byte block");
      }
    }
    *line.at = 0;
    finish();
  }
  /* The allocator cannot go on from a bad release. */
  trapNow();
}

void weavecRtDie(const char *message) {
  static const char Prefix[] = "weavec: error: ";
  char text[512];
  struct Line line = {text, text + sizeof text - 2};
  put(&line, Prefix);
  put(&line, message);
  *line.at++ = '\n';
  emit(text, (size_t)(line.at - text));
  trapNow();
}

/*===-- Faults (RFC 0037 section 6) ----------------------------------------===*/

/* The faulting instruction's address and, where the context says, whether
 * it wrote (1), read (0) or is unknown (-1). */
static uintptr_t faultPc(const void *context, int *write) {
  const ucontext_t *uc = (const ucontext_t *)context;
  *write = -1;
#if defined(__APPLE__) && defined(__aarch64__)
  /* A data abort's syndrome: bit 6 (WnR) is set by a write. */
  *write = (uc->uc_mcontext->__es.__esr >> 6) & 1;
  return (uintptr_t)arm_thread_state64_get_pc(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
  *write = (uc->uc_mcontext->__es.__err & 2) != 0;
  return (uintptr_t)uc->uc_mcontext->__ss.__rip;
#elif defined(__linux__) && defined(__x86_64__)
  *write = (uc->uc_mcontext.gregs[REG_ERR] & 2) != 0;
  return (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(__aarch64__)
  return (uintptr_t)uc->uc_mcontext.pc;
#else
  (void)uc;
  return 0;
#endif
}

/* RFC 0038 section 4: the default disposition, set here rather than left to
 * SA_RESETHAND, which a program that saved and restored this handler has
 * dropped (Python's faulthandler): raising the signal again under this
 * handler would never end. */
static void raiseDefault(int signal) {
  struct sigaction action;
  memset(&action, 0, sizeof action);
  action.sa_handler = SIG_DFL;
  sigemptyset(&action.sa_mask);
  (void)sigaction(signal, &action, NULL);
  (void)raise(signal);
}

/* The frame pointer of the interrupted code. */
static uintptr_t faultFp(const void *context) {
  const ucontext_t *uc = (const ucontext_t *)context;
#if defined(__APPLE__) && defined(__aarch64__)
  return (uintptr_t)arm_thread_state64_get_fp(uc->uc_mcontext->__ss);
#elif defined(__APPLE__) && defined(__x86_64__)
  return (uintptr_t)uc->uc_mcontext->__ss.__rbp;
#elif defined(__linux__) && defined(__x86_64__)
  return (uintptr_t)uc->uc_mcontext.gregs[REG_RBP];
#elif defined(__linux__) && defined(__aarch64__)
  return (uintptr_t)uc->uc_mcontext.regs[29];
#else
  (void)uc;
  return 0;
#endif
}

/* A fault the shadow did not see (a wild access, the inaccessible middle of
 * a huge block): one report, with the stack from the faulting instruction
 * (RFC 0040 section 5.6), then the signal again under the default
 * disposition, which ends the program as it would have when the handler
 * returns (a signal sent, which no access would retry, too). */
static void onFault(int signal, siginfo_t *info, void *context) {
  const uintptr_t address = (uintptr_t)info->si_addr;
  int write = -1;
  const uintptr_t pc = faultPc(context, &write);
  /* A signal sent (kill, raise) faulted nothing: Linux says who sent it;
   * Darwin's arm64 syndrome says the thread was in a system call. */
#if defined(__APPLE__) && defined(__aarch64__)
  const int sent = ((((const ucontext_t *)context)->uc_mcontext->__es.__esr >>
                     26) & 0x3F) == 0x15;
#else
  const int sent = info->si_code <= 0;
#endif
  if (!sent && begin("memory-fault", NULL)) {
    struct Line line = {report.what, report.what + sizeof report.what - 1};
    report.pc = pc;
    report.op = write == 1 ? "write" : write == 0 ? "read" : "access";
    report.address = address;
    put(&line, report.op);
    put(&line, " at ");
    putHex(&line, address);
    *line.at = 0;
    report.stack.count = weavecRtUnwind(report.stack.pc, WeavecRtReportFrames,
                                        faultFp(context), pc);
    report.stack.thread = weavecRtThread();
    (void)describeHeap(address);
    finish();
  }
  raiseDefault(signal);
}

/* The handlers go where the disposition is the default, so that a program's
 * own handler (installed before or after) is its own. WEAVEC_RT_FAULTS=0
 * installs none. */
__attribute__((constructor)) static void watchFaults(void) {
  static const int Faults[2] = {SIGSEGV, SIGBUS};
  const char *value = getenv("WEAVEC_RT_FAULTS");
  int i;
  if (value != NULL && strcmp(value, "0") == 0)
    return;
  for (i = 0; i < 2; ++i) {
    struct sigaction action;
    if (sigaction(Faults[i], NULL, &action) != 0 ||
        (action.sa_flags & SA_SIGINFO) != 0 || action.sa_handler != SIG_DFL)
      continue;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = onFault;
    action.sa_flags = SA_SIGINFO | SA_RESETHAND | SA_ONSTACK;
    sigemptyset(&action.sa_mask);
    (void)sigaction(Faults[i], &action, NULL);
  }
}

/*===-- Statistics ----------------------------------------------------------===*/

/* Counted without synchronization: a diagnostic aid, not an account. */
unsigned long long weavecRtStats[WeavecRtStatCount];
int weavecRtStatsOn;

/* Read once, in a constructor, as the other variables are. */
__attribute__((constructor)) static void readStats(void) {
  const char *value = getenv("WEAVEC_RT_STATS");
  weavecRtStatsOn = value != NULL && strcmp(value, "1") == 0;
}

static void statLine(struct Line *line, char *text) {
  long written;
  *line->at++ = '\n';
  written = (long)write(2, text, (size_t)(line->at - text));
  (void)written;
}

__attribute__((destructor)) static void printStats(void) {
  static const char *const Names[WeavecRtStatCount] = {
      "allocations", "releases",     "recycled slots", "huge blocks",
      "slow guards", "range guards", "string guards",  "stack unpoisons",
      "sweeps", "typed allocations", "releases to a pool",
      "pooled slots moved to the quarantine"};
  char text[400];
  struct Line line = {text, text + sizeof text - 2};
  int i;
  if (!weavecRtStatsOn)
    return;
  for (i = 0; i < WeavecRtStatCount; ++i) {
    line.at = text;
    put(&line, "weavec: runtime: ");
    putUnsigned(&line, weavecRtStats[i], 10);
    put(&line, " ");
    put(&line, Names[i]);
    statLine(&line, text);
  }
  {
    struct weavecRtSweepCounts last;
    weavecRtLastSweep(&last);
    line.at = text;
    put(&line, "weavec: runtime: sweeps took ");
    putUnsigned(&line, weavecRtSweepNanos / 1000000, 10);
    put(&line, " ms (longest ");
    putUnsigned(&line, weavecRtSweepLongest / 1000000, 10);
    put(&line, " ms); the last retained ");
    putUnsigned(&line, last.retained, 10);
    put(&line, " blocks (");
    putUnsigned(&line, last.retainedBytes, 10);
    put(&line, " bytes)");
    statLine(&line, text);
  }
  {
    size_t count;
    size_t bytes;
    weavecRtPoolStats(&count, &bytes);
    line.at = text;
    put(&line, "weavec: runtime: ");
    putUnsigned(&line, count, 10);
    put(&line, " pools holding ");
    putUnsigned(&line, bytes >> 10, 10);
    put(&line, " KiB of free slots");
    statLine(&line, text);
  }
  {
    size_t live;
    size_t quarantine;
    weavecRtPeaks(&live, &quarantine);
    line.at = text;
    put(&line, "weavec: runtime: at most ");
    putUnsigned(&line, live >> 20, 10);
    put(&line, " MiB of live slots and ");
    putUnsigned(&line, quarantine >> 20, 10);
    put(&line, " MiB of released ones waiting for a sweep");
    statLine(&line, text);
  }
  {
    /* RFC 0040 sections 2 and 7. */
    size_t entries;
    size_t bytes;
    weavecRtStoreStats(&entries, &bytes);
    line.at = text;
    put(&line, "weavec: runtime: ");
    putUnsigned(&line, entries, 10);
    put(&line, " stacks recorded (");
    putUnsigned(&line, bytes >> 10, 10);
    put(&line, " KiB)");
    statLine(&line, text);
    line.at = text;
    put(&line, "weavec: runtime: ");
    putUnsigned(&line, weavecRtUntrackedBytes() >> 20, 10);
    put(&line, " MiB of writable memory the runtime does not track (mappings "
               "the program or its libraries made: objects an allocator of "
               "their own carves from them are not checked)");
    statLine(&line, text);
  }
}

/*===-- One runtime per process (RFC 0038 section 1) -----------------------===*/

static int isRuntime(const char *path) {
  const char *slash = path != NULL ? strrchr(path, '/') : NULL;
  const char *name = slash != NULL ? slash + 1 : path;
  return name != NULL && strncmp(name, "libweavec_rt.", 13) == 0;
}

#if !defined(__APPLE__)
struct OtherRuntime {
  uintptr_t own;
  const char *found;
};

static int findOtherRuntime(struct dl_phdr_info *info, size_t size,
                            void *data) {
  struct OtherRuntime *search = (struct OtherRuntime *)data;
  (void)size;
  if (info->dlpi_addr != search->own && isRuntime(info->dlpi_name))
    search->found = info->dlpi_name;
  return search->found != NULL;
}
#endif

/* Another runtime's image in the process, or null. */
static const char *otherRuntime(const Dl_info *own) {
#if defined(__APPLE__)
  uint32_t i;
  for (i = 0; i < _dyld_image_count(); ++i)
    if ((const void *)_dyld_get_image_header(i) != own->dli_fbase &&
        isRuntime(_dyld_get_image_name(i)))
      return _dyld_get_image_name(i);
  return NULL;
#else
  struct OtherRuntime search = {(uintptr_t)own->dli_fbase, NULL};
  (void)dl_iterate_phdr(findOtherRuntime, &search);
  return search.found;
#endif
}

/* Two runtimes would mean two shadows and two heaps: images built by two
 * WeaveC versions, which the loader cannot reconcile, stop here. A process
 * whose allocator is another's (a guarded library loaded by a program
 * weavec-cc did not link, on Linux) is told so once. */
__attribute__((constructor)) static void checkProcess(void) {
  Dl_info own;
  const char *other;
  char text[1024];
  struct Line line = {text, text + sizeof text - 2};
  if (dladdr((const void *)checkProcess, &own) == 0 || own.dli_fname == NULL)
    return;
  other = otherRuntime(&own);
  /* A copy of the archive in an image another driver linked (RFC 0038
   * amendment 11) has another shadow descriptor than the one the process
   * resolves. */
  if (other == NULL) {
    Dl_info where;
    const void *shadow = dlsym(RTLD_DEFAULT, "__weavec_rt_shadow");
    if (shadow != NULL && shadow != (const void *)&__weavec_rt_shadow &&
        dladdr(shadow, &where) != 0)
      other = where.dli_fname;
  }
  if (other != NULL) {
    put(&line, "weavec: error: two WeaveC runtimes in one process (");
    put(&line, other);
    put(&line, " and ");
    put(&line, own.dli_fname);
    put(&line, "): build every image with one weavec-cc");
    *line.at++ = '\n';
    emit(text, (size_t)(line.at - text));
    _exit(127);
  }
#if !defined(__APPLE__)
  {
    const char *notes = getenv("WEAVEC_RT_NOTES");
    void *found = dlsym(RTLD_DEFAULT, "malloc");
    Dl_info where;
    Dl_info program;
    if ((notes != NULL && strcmp(notes, "0") == 0) || found == NULL ||
        dladdr(found, &where) == 0 || where.dli_fbase == own.dli_fbase)
      return;
    /* A program that defines the allocator itself was told so when it was
     * linked. */
    if (dladdr((const void *)getauxval(AT_PHDR), &program) != 0 &&
        program.dli_fbase == where.dli_fbase)
      return;
    put(&line, "weavec: note: the program's allocator is not WeaveC's (");
    put(&line, where.dli_fname != NULL ? where.dli_fname : "?");
    put(&line, " serves it: the program was not linked by weavec-cc): heap "
               "accesses are not checked; preload ");
    put(&line, own.dli_fname);
    put(&line, " to check them");
    *line.at++ = '\n';
    emit(text, (size_t)(line.at - text));
  }
#endif
}
