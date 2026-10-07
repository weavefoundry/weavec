/*===- rt_test.c - Tests of the WeaveC runtime --------------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, section 5. One program, linked with libweavec_rt.a and
|* libweavec_alloc.a, so `malloc` here is the arena's. Each test is a
|* function; a test that must stop the program runs in a child process and
|* the parent checks how it died. Run without arguments for every test, or
|* with the names of the tests to run.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "../weavec_rt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

#if !defined(__APPLE__)
/* (The runtime defines it where the C library does; libSystem has only a
 * variant symbol.) */
void *reallocarray(void *, size_t, size_t);
#endif

/* Sizes no allocator can serve, which the compiler must not see. */
static volatile size_t tooMany = (size_t)-1;

static int failures;
static const char *current;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s: check failed: %s\n", __FILE__, __LINE__,     \
              current, #condition);                                            \
      ++failures;                                                              \
    }                                                                          \
  } while (0)

/* Keeps the compiler from seeing through a pointer: the tests free and
 * index what no correct program would. */
static void *identity(void *p) { return p; }
static void *(*volatile launder)(void *) = identity;
static void *opaque(void *p) { return launder(p); }

static int inArena(const void *p) {
  return (uintptr_t)p - __weavec_rt_heap.base < __weavec_rt_heap.bytes;
}

/* Whether the n bytes at p are addressable. */
static int addressable(const void *p, size_t n) {
  return __weavec_rt_range_ok((uintptr_t)p, n);
}

static unsigned char shadowAt(const void *p) {
  return *weavecRtShadowOf((uintptr_t)p);
}

/* Runs `body` in a child; how it ended: 0 a normal exit, the signal that
 * stopped it otherwise. Its standard error goes to `log` when given. */
static int endOf(void (*body)(void), const char *log) {
  int status = 0;
  const pid_t child = fork();
  if (child == 0) {
    /* The fatal message is expected, and so is the trap: no core dump (a
     * system that hands each one to a crash reporter would take seconds). */
    const struct rlimit none = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &none);
    (void)freopen(log != NULL ? log : "/dev/null", "w", stderr);
    body();
    _exit(0);
  }
  if (child < 0 || waitpid(child, &status, 0) != child)
    return -1;
  return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}

/* True when `body` died by a trap. */
static int trapsIn(void (*body)(void)) {
  const int end = endOf(body, NULL);
  return end == SIGTRAP || end == SIGILL;
}

/* True when `body` was stopped: a trap, or the C library's abort. */
static int stopsIn(void (*body)(void)) {
  const int end = endOf(body, NULL);
  return end == SIGTRAP || end == SIGILL || end == SIGABRT || end == SIGSEGV ||
         end == SIGBUS;
}

/* The first line `body` printed on standard error. */
static void firstLine(void (*body)(void), char *line, size_t size) {
  char path[] = "/tmp/weavec_rt_test_XXXXXX";
  const int fd = mkstemp(path);
  FILE *file;
  line[0] = 0;
  if (fd < 0)
    return;
  close(fd);
  (void)endOf(body, path);
  file = fopen(path, "r");
  if (file != NULL) {
    if (fgets(line, (int)size, file) == NULL)
      line[0] = 0;
    fclose(file);
  }
  unlink(path);
}

/*===-- The shadow ----------------------------------------------------------===*/

static void testShadowReady(void) {
  void *p = opaque(malloc(1));
  CHECK(weavecRtShadowReady());
  CHECK(__weavec_rt_shadow.bits >= 39 && __weavec_rt_shadow.bits <= 48);
  /* The arena lies inside the window. */
  CHECK(weavecRtInWindow(__weavec_rt_heap.base + __weavec_rt_heap.bytes - 1));
  /* Untracked memory is addressable. */
  {
    char local[64];
    CHECK(addressable(opaque(local), sizeof local));
  }
  free(p);
}

/*===-- The allocator -------------------------------------------------------===*/

static void testClasses(void) {
  size_t size;
  /* Every size around every class boundary: the block is in the arena, its
   * bytes are addressable and zero, and the byte after it is not. */
  for (size = 0; size < 70000; size = size < 600 ? size + 1 : size + size / 7) {
    unsigned char *p = (unsigned char *)opaque(malloc(size));
    size_t i;
    CHECK(p != NULL);
    if (p == NULL)
      return;
    CHECK(inArena(p));
    CHECK(((uintptr_t)p & 15) == 0);
    for (i = 0; i < size; ++i)
      if (p[i] != 0) {
        CHECK(!"a fresh block is zero");
        break;
      }
    memset(p, 0xa5, size);
    CHECK(addressable(p, size));
    CHECK(!addressable(p + size, 1));
    CHECK(!addressable(p, size + 1));
    CHECK(__weavec_rt_size(p) == size);
    free(p);
    CHECK(!addressable(p, 1));
    CHECK(shadowAt(p) == WeavecRtShadowHeapFreed);
  }
}

static void testShadowEncoding(void) {
  unsigned char *p = (unsigned char *)opaque(malloc(20));
  CHECK(shadowAt(p) == 0);
  CHECK(shadowAt(p + 16) == 4);
  /* The slot of a 20-byte block is 32 bytes: no granule after it. */
  free(p);
  p = (unsigned char *)opaque(malloc(16));
  CHECK(shadowAt(p) == 0);
  CHECK(shadowAt(p + 16) == WeavecRtShadowHeapTail);
  free(p);
  CHECK(shadowAt(p) == WeavecRtShadowHeapFreed);
}

/* The program's path, for a test that runs in a copy of itself. */
static const char *program;

/* The quarantine budget is read when the program starts: a test that needs
 * none runs in a copy started with WEAVEC_RT_QUARANTINE=0. True in that
 * copy; in this one, runs it and checks that it passed. */
static int withoutQuarantine(void) {
  const char *budget = getenv("WEAVEC_RT_QUARANTINE");
  pid_t child;
  int status = 0;
  if (budget != NULL && strcmp(budget, "0") == 0)
    return 1;
  child = fork();
  if (child == 0) {
    char *arguments[3];
    arguments[0] = (char *)program;
    arguments[1] = (char *)current;
    arguments[2] = NULL;
    setenv("WEAVEC_RT_QUARANTINE", "0", 1);
    execv(program, arguments);
    _exit(127);
  }
  CHECK(child > 0 && waitpid(child, &status, 0) == child &&
        WIFEXITED(status) && WEXITSTATUS(status) == 0);
  return 0;
}

static void testReuseIsZero(void) {
  /* With no quarantine a released slot comes back at once, zeroed. */
  unsigned round;
  if (!withoutQuarantine())
    return;
  for (round = 0; round < 64; ++round) {
    unsigned char *p = (unsigned char *)opaque(malloc(200));
    unsigned i;
    for (i = 0; i < 200; ++i)
      CHECK(p[i] == 0);
    CHECK(addressable(p, 200) && !addressable(p, 201));
    memset(p, 0xff, 200);
    free(p);
  }
}

static void testAlignment(void) {
  size_t alignment;
  for (alignment = 16; alignment <= ((size_t)1 << 22); alignment <<= 1) {
    size_t size;
    for (size = 1; size < 3 * alignment; size = size * 2 + 5) {
      void *p = opaque(aligned_alloc(alignment, size));
      void *q = NULL;
      CHECK(p != NULL);
      CHECK(((uintptr_t)p & (alignment - 1)) == 0);
      CHECK(__weavec_rt_size(p) == size);
      CHECK(addressable(p, size));
      CHECK(posix_memalign(&q, alignment, size) == 0);
      CHECK(((uintptr_t)q & (alignment - 1)) == 0);
      free(p);
      free(q);
    }
  }
  {
    void *q = NULL;
    size_t odd = 24;
    CHECK(posix_memalign(&q, 3, 8) == EINVAL);
    CHECK(aligned_alloc((size_t)(uintptr_t)opaque((void *)odd), 8) == NULL);
  }
}

static void testRealloc(void) {
  char *p = (char *)opaque(malloc(40));
  char *same;
  char *moved;
  memset(p, 'a', 40);
  /* Within the class: the block stays, and the bytes it gains are zero. */
  same = (char *)realloc(p, 36);
  CHECK(same == p);
  CHECK(__weavec_rt_size(p) == 36);
  CHECK(addressable(p, 36) && !addressable(p, 37));
  same = (char *)opaque(realloc(p, 44));
  CHECK(same == p);
  p = same;
  CHECK(p[35] == 'a' && p[36] == 0 && p[43] == 0);
  CHECK(addressable(p, 44) && !addressable(p, 45));
  /* Out of the class: a new block, and the old one is released. */
  moved = (char *)opaque(realloc(p, 5000));
  CHECK(moved != p);
  CHECK(moved[0] == 'a' && moved[35] == 'a' && moved[36] == 0 &&
        moved[4999] == 0);
  CHECK(!addressable(p, 1));
  CHECK(addressable(moved, 5000));
  /* realloc(p, 0) releases and returns an empty block. */
  p = (char *)opaque(realloc(moved, 0));
  CHECK(p != NULL && __weavec_rt_size(p) == 0);
  CHECK(!addressable(moved, 1));
  CHECK(!addressable(p, 1));
  free(p);
  p = (char *)realloc(NULL, 10);
  CHECK(p != NULL && __weavec_rt_size(p) == 10);
  free(p);
  CHECK(opaque(calloc(tooMany, 16)) == NULL);
#if !defined(__APPLE__)
  CHECK(opaque(reallocarray(NULL, tooMany, 16)) == NULL);
#endif
}

static void testQuarantine(void) {
  /* With no budget the newest release is the first to be reused. */
  enum { Blocks = 64 };
  void *blocks[Blocks];
  unsigned i;
  void *again;
  if (!withoutQuarantine())
    return;
  for (i = 0; i < Blocks; ++i)
    blocks[i] = opaque(malloc(1000));
  for (i = 0; i < Blocks; ++i)
    free(blocks[i]);
  again = opaque(malloc(1000));
  CHECK(addressable(again, 1000));
  free(again);
}

static void testQuarantineHolds(void) {
  /* Under the default budget a released block stays released. */
  void *p = opaque(malloc(1000));
  unsigned i;
  free(p);
  for (i = 0; i < 100; ++i)
    free(opaque(malloc(1000)));
  CHECK(!addressable(p, 1));
}

static void doubleFree(void) {
  void *p = opaque(malloc(24));
  free(p);
  free(p);
}

static void interiorFree(void) {
  char *p = (char *)opaque(malloc(24));
  free(opaque(p + 8));
}

static void stackFree(void) {
  char local[16] = {0};
  free(opaque(local));
}

static int aGlobal[8];

static void globalFree(void) { free(opaque(aGlobal)); }

static void reallocDead(void) {
  void *p = opaque(malloc(24));
  free(p);
  (void)opaque(realloc(p, 100));
}

static void wildFree(void) {
  /* Inside the arena, where nothing was ever allocated. */
  free((void *)(__weavec_rt_heap.base + __weavec_rt_heap.bytes - 4096));
}

static void testInvalidReleases(void) {
  CHECK(trapsIn(doubleFree));
  CHECK(trapsIn(interiorFree));
  CHECK(trapsIn(reallocDead));
  CHECK(trapsIn(wildFree));
  /* Outside the arena a release goes to the next allocator, which stops
   * the program (glibc aborts; Darwin's zones trap). */
  CHECK(stopsIn(stackFree));
  CHECK(stopsIn(globalFree));
}

static void testHuge(void) {
  const size_t size = ((size_t)1 << 30) + 12345;
  char *p = (char *)opaque(malloc(size));
  char *q;
  CHECK(p != NULL);
  if (p == NULL)
    return;
  CHECK(!inArena(p));
  p[0] = 1;
  p[size - 1] = 2;
  CHECK(addressable(p + size - 64, 64));
  CHECK(!addressable(p + size - 1, 2));
  CHECK(__weavec_rt_size(p) == size);
  q = (char *)opaque(realloc(p, 64));
  CHECK(inArena(q) && q[0] == 1);
  /* The released mapping is a released object, not someone else's. */
  CHECK(!addressable(p, 1));
  free(q);
  p = (char *)opaque(aligned_alloc((size_t)1 << 21, ((size_t)1 << 30) + 1));
  CHECK(p != NULL && ((uintptr_t)p & (((size_t)1 << 21) - 1)) == 0);
  free(p);
  CHECK(opaque(malloc(tooMany / 2)) == NULL);
}

static void testForeign(void) {
  /* Blocks of another allocator go back to it. */
#if defined(__APPLE__)
  /* (The default zone is the arena's: another zone stands for the
   * system's.) */
  malloc_zone_t *other = malloc_create_zone(0, 0);
  char *system = (char *)malloc_zone_malloc(other, 100);
  char *line = NULL;
  size_t capacity = 0;
  FILE *file;
  CHECK(!inArena(system));
  CHECK(addressable(system, 1000));
  CHECK(malloc_size(system) >= 100);
  system = (char *)realloc(system, 5000);
  CHECK(system != NULL);
  free(system);
  /* The system library reallocates and sizes an arena block through the
   * zone: getline grows the program's own buffer. */
  line = (char *)malloc(2);
  capacity = 2;
  file = fopen("/etc/hosts", "r");
  if (file != NULL) {
    CHECK(getline(&line, &capacity, file) >= 0);
    CHECK(inArena(line));
    fclose(file);
  }
  free(line);
  line = strdup("a string from the system");
  CHECK(inArena(line));
  free(line);
#else
  /* Everything the C library allocates is the arena's: the definition in
   * this executable serves it too. */
  char *copy = strdup("a string from the C library");
  char *line = NULL;
  size_t capacity = 0;
  FILE *file = fopen("/etc/hostname", "r");
  CHECK(inArena(copy));
  CHECK(malloc_usable_size(copy) == strlen(copy) + 1);
  free(copy);
  if (file != NULL) {
    (void)getline(&line, &capacity, file);
    CHECK(line == NULL || inArena(line));
    fclose(file);
  }
  free(line);
#endif
}

/*===-- Guards --------------------------------------------------------------===*/

static const struct __weavec_rt_site trapSite = {"rt_test.c", 1, 2, 0};
static const struct __weavec_rt_site reportSite = {"rt_test.c", 3, 4,
                                                   WeavecRtSiteReport};

static void overflowRead(void) {
  char *p = (char *)opaque(malloc(20));
  __weavec_rt_guard((uintptr_t)p + 16, 8, &trapSite);
}

static void useAfterFree(void) {
  char *p = (char *)opaque(malloc(20));
  free(p);
  __weavec_rt_guard((uintptr_t)p, 4, &trapSite);
}

static void reportedOverflow(void) {
  char *p = (char *)opaque(malloc(20));
  __weavec_rt_range((uintptr_t)p, 21, &reportSite);
  __weavec_rt_range((uintptr_t)p, 21, &reportSite);
  _exit(7);
}

static void nullBase(void) { __weavec_rt_null(64, &trapSite); }

static void nullRead(void) {
  __weavec_rt_guard((uintptr_t)opaque(NULL) + 8, 4, &trapSite);
}

static void nullString(void) {
  (void)__weavec_rt_strlen(opaque(NULL), ~(uint64_t)0, &trapSite);
}

static void testGuards(void) {
  char line[256];
  char *p = (char *)opaque(malloc(20));
  /* Passing guards return. */
  __weavec_rt_guard((uintptr_t)p, 20, &trapSite);
  __weavec_rt_guard((uintptr_t)p + 19, 1, &trapSite);
  __weavec_rt_range((uintptr_t)p, 0, &trapSite);
  free(p);
  CHECK(trapsIn(overflowRead));
  CHECK(trapsIn(useAfterFree));
  CHECK(trapsIn(nullBase));
  /* The lowest page is poisoned: a guard near null fails as one. */
  CHECK(trapsIn(nullRead));
  firstLine(nullRead, line, sizeof line);
  CHECK(strstr(line, "weavec: null-dereference at rt_test.c:1:2") == line);
  firstLine(overflowRead, line, sizeof line);
  CHECK(strstr(line, "weavec: heap-buffer-overflow at rt_test.c:1:2: read "
                     "of 8 bytes") == line);
  firstLine(useAfterFree, line, sizeof line);
  CHECK(strstr(line, "weavec: heap-use-after-free at rt_test.c:1:2") == line);
  /* Report mode: once per site, and the program goes on. */
  CHECK(endOf(reportedOverflow, NULL) == 0);
  firstLine(reportedOverflow, line, sizeof line);
  CHECK(strstr(line, "weavec: heap-buffer-overflow at rt_test.c:3:4") == line);
}

static void unterminated(void) {
  char *p = (char *)opaque(malloc(16));
  memset(p, 'x', 16);
  (void)__weavec_rt_strlen(p, ~(uint64_t)0, &trapSite);
}

static void testStrings(void) {
  char *p = (char *)opaque(malloc(40));
  char local[8] = "abc";
  strcpy(p, "a string of thirty-five characters!");
  CHECK(__weavec_rt_strlen(p, ~(uint64_t)0, &trapSite) == 35);
  CHECK(__weavec_rt_strlen(p, 4, &trapSite) == 4);
  CHECK(__weavec_rt_strlen(p + 30, ~(uint64_t)0, &trapSite) == 5);
  CHECK(__weavec_rt_strlen(opaque(local), ~(uint64_t)0, &trapSite) == 3);
  CHECK(__weavec_rt_strlen("literal", ~(uint64_t)0, &trapSite) == 7);
  free(p);
  CHECK(trapsIn(unterminated));
  /* A null string is the call's to accept, or an access through null. */
  {
    static const struct __weavec_rt_site nullOk = {"rt_test.c", 5, 6,
                                                   WeavecRtSiteNullOk};
    CHECK(__weavec_rt_strlen(NULL, ~(uint64_t)0, &nullOk) == 0);
  }
  CHECK(trapsIn(nullString));
}

static void copyTooLong(void) {
  char *p = (char *)opaque(malloc(8));
  (void)__weavec_rt_strcpy(p, "eight chars", &trapSite);
}

static void printTooLong(void) {
  char *p = (char *)opaque(malloc(8));
  (void)__weavec_rt_sprintf(&trapSite, p, "%d-%d", 12345, 6789);
}

static void compareTooFar(void) {
  char *p = (char *)opaque(malloc(4));
  memcpy(p, "abcd", 4);
  (void)__weavec_rt_strcmp(p, "abcde", &trapSite);
}

static void searchTooFar(void) {
  char *p = (char *)opaque(malloc(4));
  memcpy(p, "abcd", 4);
  (void)__weavec_rt_memchr(p, 'z', 8, &trapSite);
}

static void printUnterminated(void) {
  char out[64];
  char *p = (char *)opaque(malloc(4));
  memcpy(p, "abcd", 4);
  (void)__weavec_rt_snprintf(&trapSite, out, sizeof out, "[%s]", p);
}

static void testCheckedCalls(void) {
  char *p = (char *)opaque(malloc(16));
  char *q;
  CHECK(__weavec_rt_strcpy(p, "short", &trapSite) == p);
  CHECK(strcmp(p, "short") == 0);
  CHECK(__weavec_rt_strcat(p, "er", &trapSite) == p);
  CHECK(strcmp(p, "shorter") == 0);
  q = __weavec_rt_stpcpy(p, "abc", &trapSite);
  CHECK(q == p + 3 && *q == 0);
  CHECK(__weavec_rt_sprintf(&trapSite, p, "%d", 42) == 2);
  CHECK(strcmp(p, "42") == 0);
  /* A size larger than the buffer is no error when the output fits. */
  CHECK(__weavec_rt_snprintf(&trapSite, p, 1000, "%s", "fits") == 4);
  CHECK(strcmp(p, "fits") == 0);
  CHECK(__weavec_rt_snprintf(&trapSite, p, 4, "%s", "truncated") == 9);
  CHECK(strcmp(p, "tru") == 0);
  /* A search or comparison checks only what it read. */
  memcpy(p, "ab\nc", 4);
  CHECK(__weavec_rt_memchr(p, '\n', 1000, &trapSite) == p + 2);
  CHECK(__weavec_rt_strchr("abc", 'b', &trapSite) != NULL);
  CHECK(__weavec_rt_strchr("abc", 'z', &trapSite) == NULL);
  CHECK(__weavec_rt_strcmp("abc", "abd", &trapSite) < 0);
  CHECK(__weavec_rt_strcmp("abc", "abc", &trapSite) == 0);
  CHECK(__weavec_rt_strncmp("abcx", "abcy", 3, &trapSite) == 0);
  CHECK(__weavec_rt_strncmp("b", "a", 1000, &trapSite) > 0);
  CHECK(__weavec_rt_strcasecmp("Content-Type", "content-type", &trapSite) == 0);
  CHECK(__weavec_rt_strncasecmp("HEADx", "heady", 4, &trapSite) == 0);
  CHECK(__weavec_rt_strcasecmp("a", "B", &trapSite) < 0);
  /* A range check reads the shadow eight granules at a time. */
  {
    char *big = (char *)opaque(malloc(1000));
    CHECK(__weavec_rt_range_ok((uintptr_t)big, 1000));
    CHECK(__weavec_rt_range_ok((uintptr_t)big + 3, 997));
    CHECK(!__weavec_rt_range_ok((uintptr_t)big, 1001));
    CHECK(!__weavec_rt_range_ok((uintptr_t)big + 999, 2));
    free(big);
    CHECK(!__weavec_rt_range_ok((uintptr_t)big, 16));
  }
  free(p);
  CHECK(trapsIn(copyTooLong));
  CHECK(trapsIn(printTooLong));
  CHECK(trapsIn(compareTooFar));
  /* A format's %s arguments are checked, up to their precision. */
  CHECK(trapsIn(printUnterminated));
  {
    char out[16];
    char *word = (char *)opaque(malloc(4));
    memcpy(word, "abcd", 4);
    CHECK(__weavec_rt_snprintf(&trapSite, out, sizeof out, "%.4s|%d|%s",
                               word, 7, (const char *)NULL) > 0);
    CHECK(strncmp(out, "abcd|7|", 7) == 0);
    free(word);
  }
  CHECK(trapsIn(searchTooFar));
}

/*===-- Frames and globals --------------------------------------------------===*/

static unsigned char aTrackedGlobal[64] __attribute__((aligned(32)));

static void testGlobals(void) {
  const struct __weavec_rt_global global = {(uintptr_t)aTrackedGlobal, 20, 64};
  __weavec_rt_globals_register(&global, 1);
  CHECK(addressable(aTrackedGlobal, 20));
  CHECK(!addressable(aTrackedGlobal, 21));
  CHECK(!addressable(aTrackedGlobal + 40, 1));
  __weavec_rt_globals_unregister(&global, 1);
  CHECK(addressable(aTrackedGlobal, 64));
}

static void testAllocas(void) {
  unsigned char buffer[128] __attribute__((aligned(32)));
  const uintptr_t at = (uintptr_t)opaque(buffer);
  __weavec_rt_alloca_poison(at, 40);
  CHECK(addressable(buffer, 40));
  CHECK(!addressable(buffer + 40, 1));
  CHECK(!addressable(buffer + 64, 1));
  __weavec_rt_alloca_unpoison(at, at + sizeof buffer);
  CHECK(addressable(buffer, sizeof buffer));
}

static jmp_buf unwound;
static uintptr_t poisoned;

static __attribute__((noinline)) void deep(void) {
  unsigned char frame[64] __attribute__((aligned(32)));
  poisoned = (uintptr_t)opaque(frame);
  weavecRtShadowSet(poisoned, 64, WeavecRtShadowStackMid);
  CHECK(!addressable(frame, 1));
  /* What a guarded program does before a call that does not return. */
  __weavec_rt_unpoison_stack();
  longjmp(unwound, 1);
}

static void testUnpoisonStack(void) {
  if (setjmp(unwound) == 0)
    deep();
  CHECK(addressable((void *)poisoned, 64));
}

static void testMappings(void) {
  void *map = __weavec_rt_mmap(NULL, 1 << 16, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0, NULL);
  CHECK(map != MAP_FAILED);
  if (map == MAP_FAILED)
    return;
  weavecRtShadowSet((uintptr_t)map, 1 << 16, WeavecRtShadowStackMid);
  CHECK(!addressable(map, 1));
  CHECK(__weavec_rt_munmap(map, 1 << 16, NULL) == 0);
  CHECK(addressable(map, 1 << 16));
}

/*===-- Array bounds --------------------------------------------------------===*/

struct OutOfBounds {
  const char *file;
  unsigned line;
  unsigned column;
  const void *arrayType;
  const void *indexType;
};

void __ubsan_handle_out_of_bounds(void *data, uintptr_t index);

static void boundsReported(void) {
  static const struct OutOfBounds data = {"array.c", 5, 6, NULL, NULL};
  __ubsan_handle_out_of_bounds((void *)&data, 9);
  _exit(0);
}

static void testArrayBounds(void) {
  char line[256];
  firstLine(boundsReported, line, sizeof line);
  CHECK(strstr(line, "weavec: index-out-of-bounds at array.c:5:6: index 9") ==
        line);
}

/*===-- Threads and processes -----------------------------------------------===*/

static void *worker(void *argument) {
  unsigned seed = (unsigned)(uintptr_t)argument;
  void *held[64] = {0};
  unsigned round;
  for (round = 0; round < 20000; ++round) {
    const unsigned slot = (seed = seed * 1103515245u + 12345u) >> 16 & 63;
    const size_t size = (seed >> 8) % 3000;
    if (held[slot] != NULL) {
      if (!addressable(held[slot], __weavec_rt_size(held[slot])))
        return (void *)1;
      free(held[slot]);
    }
    held[slot] = malloc(size);
    if (held[slot] == NULL || __weavec_rt_size(held[slot]) != size)
      return (void *)1;
    memset(held[slot], (int)slot, size);
  }
  for (round = 0; round < 64; ++round)
    free(held[round]);
  return NULL;
}

static void testThreads(void) {
  enum { Threads = 8 };
  pthread_t threads[Threads];
  unsigned i;
  for (i = 0; i < Threads; ++i)
    CHECK(pthread_create(&threads[i], NULL, worker,
                         (void *)(uintptr_t)(i + 1)) == 0);
  for (i = 0; i < Threads; ++i) {
    void *result = (void *)1;
    CHECK(pthread_join(threads[i], &result) == 0);
    CHECK(result == NULL);
  }
}

static void testFork(void) {
  void *p = opaque(malloc(100));
  int status = 0;
  const pid_t child = fork();
  if (child == 0) {
    void *q = malloc(100);
    free(p);
    free(q);
    _exit(q != NULL ? 0 : 1);
  }
  CHECK(child > 0);
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(addressable(p, 100));
  free(p);
}

/*===-- main ----------------------------------------------------------------===*/

static const struct {
  const char *name;
  void (*run)(void);
} Tests[] = {
    {"shadow-ready", testShadowReady},
    {"classes", testClasses},
    {"shadow-encoding", testShadowEncoding},
    {"reuse-is-zero", testReuseIsZero},
    {"alignment", testAlignment},
    {"realloc", testRealloc},
    {"quarantine", testQuarantine},
    {"quarantine-holds", testQuarantineHolds},
    {"invalid-releases", testInvalidReleases},
    {"huge", testHuge},
    {"foreign", testForeign},
    {"guards", testGuards},
    {"strings", testStrings},
    {"checked-calls", testCheckedCalls},
    {"globals", testGlobals},
    {"allocas", testAllocas},
    {"unpoison-stack", testUnpoisonStack},
    {"mappings", testMappings},
    {"array-bounds", testArrayBounds},
    {"threads", testThreads},
    {"fork", testFork},
};

int main(int argc, char **argv) {
  unsigned i;
  unsigned ran = 0;
  program = argv[0];
  for (i = 0; i < sizeof Tests / sizeof Tests[0]; ++i) {
    int selected = argc < 2;
    int a;
    for (a = 1; a < argc; ++a)
      selected |= strcmp(argv[a], Tests[i].name) == 0;
    if (!selected)
      continue;
    current = Tests[i].name;
    Tests[i].run();
    ++ran;
  }
  if (ran == 0) {
    fprintf(stderr, "rt_test: no such test\n");
    return 2;
  }
  if (failures != 0) {
    fprintf(stderr, "rt_test: %d check%s failed\n", failures,
            failures == 1 ? "" : "s");
    return 1;
  }
  printf("rt_test: %u tests passed\n", ran);
  return 0;
}
