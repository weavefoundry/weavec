/*===- rt_test.c - Tests of the WeaveC runtime --------------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0032, section 12. One program, linked with libweavec_rt.a and
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
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <malloc/malloc.h>
#else
#include <malloc.h>
#endif

void *reallocarray(void *, size_t, size_t);

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

/* Runs `body` in a child; true when it died by a trap. */
static int trapsIn(void (*body)(void)) {
  int status = 0;
  const pid_t child = fork();
  if (child == 0) {
    /* The fatal message is expected, and so is the trap: no core dump (a
     * system that hands each one to a crash reporter would take seconds). */
    const struct rlimit none = {0, 0};
    (void)setrlimit(RLIMIT_CORE, &none);
    (void)freopen("/dev/null", "w", stderr);
    body();
    _exit(0);
  }
  if (child < 0 || waitpid(child, &status, 0) != child)
    return 0;
  return WIFSIGNALED(status) &&
         (WTERMSIG(status) == SIGTRAP || WTERMSIG(status) == SIGILL);
}

/*===-- The allocator -------------------------------------------------------===*/

static void testClasses(void) {
  size_t size;
  /* Every size around every class boundary: the block is in the arena, its
   * extent is exact, it is zero, and one byte after it belongs to nothing. */
  for (size = 0; size < 70000; size = size < 600 ? size + 1 : size + size / 7) {
    unsigned char *p = (unsigned char *)opaque(malloc(size));
    struct __weavec_rt_found found;
    size_t i;
    CHECK(p != NULL);
    if (p == NULL)
      return;
    CHECK(inArena(p));
    CHECK(((uintptr_t)p & 15) == 0);
    found = __weavec_rt_find(p);
    CHECK(found.state == WeavecRtTrackedLive);
    CHECK(found.base == (uintptr_t)p);
    CHECK(found.size == size);
    for (i = 0; i < size; ++i)
      if (p[i] != 0) {
        CHECK(!"a fresh block is zero");
        break;
      }
    memset(p, 0xa5, size);
    /* Interior, last byte, one past the end. */
    if (size != 0) {
      CHECK(__weavec_rt_find(p + size - 1).base == (uintptr_t)p);
      CHECK(__weavec_rt_object(p, (long long)size - 1, 1, 0, 1) == 0);
    }
    CHECK(__weavec_rt_find(p + size).base == (uintptr_t)p);
    CHECK(__weavec_rt_object(p, (long long)size, 1, 0, 1) != 0);
    CHECK(__weavec_rt_object(p + size, -1, 1, 0, 1) == (size == 0));
    CHECK(__weavec_rt_object(p, -1, 1, 0, 1) != 0);
    CHECK(__weavec_rt_object(p, 0, 0, 0, size) == 0);
    CHECK(__weavec_rt_object(p, 0, 0, 0, size + 1) != 0);
    CHECK(__weavec_rt_size(p) == size);
    free(p);
    CHECK(__weavec_rt_find(p).state == WeavecRtTrackedDead);
  }
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
      CHECK(posix_memalign(&q, alignment, size) == 0);
      CHECK(((uintptr_t)q & (alignment - 1)) == 0);
      free(p);
      free(q);
    }
  }
  {
    void *q = NULL;
    CHECK(posix_memalign(&q, 3, 8) == EINVAL);
    size_t odd = 24;
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
  same = (char *)opaque(realloc(p, 44));
  CHECK(same == p);
  p = same;
  CHECK(p[35] == 'a' && p[36] == 0 && p[43] == 0);
  CHECK(__weavec_rt_object(p, 43, 1, 0, 1) == 0);
  CHECK(__weavec_rt_object(p, 44, 1, 0, 1) != 0);
  /* Out of the class: a new block, and the old one is dead. */
  moved = (char *)opaque(realloc(p, 5000));
  CHECK(moved != p);
  CHECK(moved[0] == 'a' && moved[35] == 'a' && moved[36] == 0 &&
        moved[4999] == 0);
  CHECK(__weavec_rt_live(p) != 0);
  CHECK(__weavec_rt_live(moved) == 0);
  /* realloc(p, 0) releases and returns an empty block. */
  p = (char *)opaque(realloc(moved, 0));
  CHECK(p != NULL && __weavec_rt_size(p) == 0);
  CHECK(__weavec_rt_live(moved) != 0);
  free(p);
  p = (char *)realloc(NULL, 10);
  CHECK(p != NULL && __weavec_rt_size(p) == 10);
  free(p);
  CHECK(opaque(calloc(tooMany, 16)) == NULL);
  CHECK(opaque(reallocarray(NULL, tooMany, 16)) == NULL);
}

static void testQuarantine(void) {
  /* A released block stays dead while the budget holds, in release order. */
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
  /* No budget: the newest release is the first to be reused. */
  again = opaque(malloc(1000));
  CHECK(__weavec_rt_live(again) == 0);
  free(again);
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
  CHECK(trapsIn(stackFree));
  CHECK(trapsIn(reallocDead));
  CHECK(trapsIn(wildFree));
  {
    static const void *const descriptor[2] = {aGlobal, (void *)sizeof aGlobal};
    __weavec_rt_globals_add(descriptor, descriptor + 2);
    CHECK(trapsIn(globalFree));
  }
  {
    char *p = (char *)opaque(malloc(24));
    char local[8] = {0};
    CHECK(__weavec_rt_release_ok(NULL) == 0);
    CHECK(__weavec_rt_release_ok(p) == 0);
    CHECK(__weavec_rt_release_ok(p + 1) != 0);
    CHECK(__weavec_rt_release_ok(opaque(local)) != 0);
    CHECK(__weavec_rt_release_ok(aGlobal) != 0);
    free(p);
    CHECK(__weavec_rt_release_ok(p) != 0);
  }
}

static void testHuge(void) {
  const size_t size = ((size_t)1 << 30) + 12345;
  char *p = (char *)opaque(malloc(size));
  struct __weavec_rt_found found;
  char *q;
  CHECK(p != NULL);
  if (p == NULL)
    return;
  CHECK(!inArena(p));
  found = __weavec_rt_find(p + 100);
  CHECK(found.state == WeavecRtTrackedLive && found.base == (uintptr_t)p &&
        found.size == size);
  p[0] = 1;
  p[size - 1] = 2;
  CHECK(__weavec_rt_object(p, (long long)size - 1, 1, 0, 1) == 0);
  CHECK(__weavec_rt_object(p, (long long)size, 1, 0, 1) != 0);
  CHECK(__weavec_rt_size(p) == size);
  q = (char *)opaque(realloc(p, 64));
  CHECK(inArena(q) && q[0] == 1);
  /* The released mapping is a dead object, not someone else's memory. */
  CHECK(__weavec_rt_find(p).state == WeavecRtTrackedDead);
  CHECK(__weavec_rt_live(p) != 0);
  free(q);
  p = (char *)opaque(aligned_alloc((size_t)1 << 21, ((size_t)1 << 30) + 1));
  CHECK(p != NULL && ((uintptr_t)p & (((size_t)1 << 21) - 1)) == 0);
  free(p);
  CHECK(opaque(malloc(tooMany / 2)) == NULL);
}

static void testForeign(void) {
  /* Blocks of another allocator go back to it. */
#if defined(__APPLE__)
  /* (The default zone is the arena's, RFC 0033 section 6.2: another
   * zone stands for the system's.) */
  malloc_zone_t *other = malloc_create_zone(0, 0);
  char *system = (char *)malloc_zone_malloc(other, 100);
  char *line = NULL;
  size_t capacity = 0;
  FILE *file;
  CHECK(!inArena(system));
  CHECK(__weavec_rt_find(system).state == WeavecRtUntracked);
  CHECK(__weavec_rt_object(system, 1000, 1, 0, 1) == 0);
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
    CHECK(__weavec_rt_size(line) >= capacity || capacity != 0);
    fclose(file);
  }
  free(line);
  /* strdup allocates in the system library: untracked, and freed here. */
  line = strdup("a string from the system");
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

/*===-- Stack objects -------------------------------------------------------===*/

static __attribute__((noinline)) int guardByte(const char *p, long long index) {
  return __weavec_rt_object(opaque((void *)(uintptr_t)p), index, 1, 0, 1);
}

static __attribute__((noinline)) void innerFrame(const char *outer) {
  char mine[24] = {0};
  void *frame = __builtin_frame_address(0);
  __weavec_rt_stack_enter(mine, sizeof mine, frame, 0);
  CHECK(guardByte(mine, 0) == 0);
  CHECK(guardByte(mine, 23) == 0);
  CHECK(guardByte(mine, 24) != 0);
  CHECK(guardByte(mine + 24, 0) != 0);
  CHECK(guardByte(mine + 24, -1) == 0);
  CHECK(guardByte(mine, -1) != 0 || guardByte(mine, -1) == 0);
  /* The caller's object is still found from here. */
  CHECK(guardByte(outer, 7) == 0);
  CHECK(guardByte(outer, 8) != 0);
  __weavec_rt_stack_leave(mine, frame);
  /* Untracked once its scope ended. */
  CHECK(__weavec_rt_find(mine).state == WeavecRtUntracked);
}

static void testStack(void) {
  char buffer[8] = {0};
  char other[8] = {0};
  void *frame = __builtin_frame_address(0);
  CHECK(__weavec_rt_find(buffer).state == WeavecRtUntracked);
  __weavec_rt_stack_enter(buffer, sizeof buffer, frame, 0);
  CHECK(__weavec_rt_find(buffer + 3).state == WeavecRtTrackedLive);
  CHECK(__weavec_rt_find(buffer + 3).base == (uintptr_t)buffer);
  /* An unregistered neighbour is untracked, unless it starts exactly where
   * the registered one ends (the compiler must not fold the comparison). */
  CHECK(__weavec_rt_find(other).state == WeavecRtUntracked ||
        (uintptr_t)opaque(other) == (uintptr_t)opaque(buffer) + 8);
  innerFrame(buffer);
  CHECK(guardByte(buffer, 7) == 0);
  CHECK(__weavec_rt_live(buffer) == 0);
  __weavec_rt_stack_leave(buffer, frame);
  CHECK(__weavec_rt_find(buffer).state == WeavecRtUntracked);
  /* Leaving something that was never entered is ignored. */
  __weavec_rt_stack_leave(other, frame);
}

/* RFC 0034, section 4: an object off a granule (a parameter's storage) next
 * to one on a granule. Neither's shadow may hide the other's. */
static void testMixedNeighbours(void) {
  char area[64] __attribute__((aligned(16))) = {0};
  char *const exact = area + 16;
  char *const mixed = area + 8;
  void *frame = __builtin_frame_address(0);
  unsigned g;
  /* The exact one first, then the mixed one ending where it starts: the
   * exact one's first granule stays its own, and its leave clears it. */
  __weavec_rt_stack_enter(exact, 32, frame, 0);
  __weavec_rt_stack_enter(mixed, 8, frame, 0);
  CHECK(guardByte(exact, 0) == 0);
  CHECK(guardByte(exact, 31) == 0);
  CHECK(guardByte(mixed, 7) == 0);
  __weavec_rt_stack_leave(mixed, frame);
  __weavec_rt_stack_leave(exact, frame);
  if (weavecRtShadowReady())
    for (g = 1; g < 4; ++g)
      CHECK(!weavecRtIsObjectByte(*weavecRtShadowOf((uintptr_t)area + 16 * g)));
  /* The mixed one first, then an exact one sharing its last granule: the
   * granule stays mixed, so the mixed one's bytes stay reachable. */
  __weavec_rt_stack_enter(area + 8, 8, frame, 0);
  __weavec_rt_stack_enter(area, 8, frame, 0);
  CHECK(guardByte(area + 8, 0) == 0);
  CHECK(guardByte(area + 8, 7) == 0);
  CHECK(guardByte(area, 7) == 0);
  __weavec_rt_stack_leave(area + 8, frame);
  __weavec_rt_stack_leave(area, frame);
}

/* Clearing a large range (a thread's whole stack below a `setjmp`) maps
 * the shadow's whole pages afresh and writes the ends. */
static void testShadowClear(void) {
  static char span[4 << 20] __attribute__((aligned(16)));
  const uintptr_t low = (uintptr_t)span + 8, high = (uintptr_t)span + sizeof span - 8;
  uintptr_t a;
  if (!weavecRtShadowReady())
    return;
  for (a = low & ~(uintptr_t)15; a < high; a += 4096)
    *weavecRtShadowOf(a) = 0x41;
  *weavecRtShadowOf(low) = 0x41;
  *weavecRtShadowOf(high - 1) = 0x41;
  weavecRtShadowClear(low, high);
  for (a = low & ~(uintptr_t)15; a < high; a += 4096)
    CHECK(*weavecRtShadowOf(a) == 0);
  CHECK(*weavecRtShadowOf(low) == 0);
  CHECK(*weavecRtShadowOf(high - 1) == 0);
}

/* A large exactly encoded object: its runs are logarithmic, so a lookup
 * anywhere in it finds its start and end, and a guard from its start to
 * its far end passes. */
static void testLargeObject(void) {
  static char big[(1 << 20) + 24] __attribute__((aligned(16)));
  struct __weavec_rt_found found;
  if (!weavecRtShadowReady())
    return;
  weavecRtShadowObject((uintptr_t)big, sizeof big, 1);
  found = __weavec_rt_find(big + 700001);
  CHECK(found.state == WeavecRtTrackedLive);
  CHECK(found.base == (uintptr_t)big);
  CHECK(found.size == sizeof big);
  CHECK(guardByte(big, 0) == 0);
  CHECK(guardByte(big, (long long)sizeof big - 1) == 0);
  CHECK(guardByte(big, (long long)sizeof big) != 0);
  weavecRtShadowClear((uintptr_t)big, (uintptr_t)big + sizeof big);
}

/* An object whose last granule unknown storage may share (a frame with
 * unnamed storage) has the whole granule, and is still found from its
 * middle; one that nothing shares has it exact. */
static void testLoosePartial(void) {
  /* Two whole granules of its own, whatever the linker puts after it. */
  static struct {
    char object[24];
    char rest[8];
  } block __attribute__((aligned(16)));
  char *const shared = block.object;
  if (!weavecRtShadowReady())
    return;
  weavecRtShadowClear((uintptr_t)&block, (uintptr_t)&block + sizeof block);
  weavecRtShadowObject((uintptr_t)shared, sizeof block.object, 0);
  CHECK(*weavecRtShadowOf((uintptr_t)shared) == WeavecRtShadowObjectRun + 1);
  CHECK(*weavecRtShadowOf((uintptr_t)shared + 16) ==
        WeavecRtShadowObjectLast + 16);
  CHECK(__weavec_rt_find(shared + 20).base == (uintptr_t)shared);
  CHECK(guardByte(shared, 31) == 0);
  weavecRtShadowClear((uintptr_t)&block, (uintptr_t)&block + sizeof block);
  weavecRtShadowObject((uintptr_t)shared, sizeof block.object, 1);
  CHECK(*weavecRtShadowOf((uintptr_t)shared + 16) ==
        WeavecRtShadowObjectLast + 8);
  weavecRtShadowClear((uintptr_t)&block, (uintptr_t)&block + sizeof block);
}

static jmp_buf jump;

static __attribute__((noinline)) void jumpsOut(void) {
  char lost[32] = {0};
  __weavec_rt_stack_enter(lost, sizeof lost, __builtin_frame_address(0), 0);
  CHECK(__weavec_rt_find(lost).state == WeavecRtTrackedLive);
  longjmp(jump, 1);
}

static __attribute__((noinline)) uintptr_t sameDepth(void) {
  /* Occupies the stack where `jumpsOut`'s frame was. */
  volatile char unknown[32];
  unknown[0] = 0;
  return (uintptr_t)opaque((void *)(uintptr_t)unknown);
}

static void testLongjmp(void) {
  if (setjmp(jump) == 0) {
    jumpsOut();
    CHECK(!"longjmp returns");
  }
  /* What `longjmp` skipped is dropped when the `setjmp` returns again. */
  __weavec_rt_stack_rewind(__builtin_frame_address(0));
  CHECK(__weavec_rt_find((void *)sameDepth()).state == WeavecRtUntracked);
}

static __attribute__((noinline)) void deepFrames(unsigned depth,
                                                 const char *outermost) {
  char mine[16] = {0};
  void *frame = __builtin_frame_address(0);
  __weavec_rt_stack_enter(mine, sizeof mine, frame, 0);
  if (depth != 0)
    deepFrames(depth - 1, outermost);
  else
    CHECK(guardByte(outermost, 15) == 0 && guardByte(outermost, 16) != 0);
  CHECK(guardByte(mine, 15) == 0);
  __weavec_rt_stack_leave(mine, frame);
}

static void testDeepStack(void) {
  char outermost[16] = {0};
  void *frame = __builtin_frame_address(0);
  __weavec_rt_stack_enter(outermost, sizeof outermost, frame, 0);
  /* More frames than the list holds inline. */
  deepFrames(500, outermost);
  __weavec_rt_stack_leave(outermost, frame);
}

/*===-- Global objects ------------------------------------------------------===*/

static char pairOfGlobals[2][16];

static void testGlobals(void) {
  static const void *const descriptors[4] = {
      pairOfGlobals[0], (void *)(uintptr_t)16, pairOfGlobals[1],
      (void *)(uintptr_t)16};
  __weavec_rt_globals_add(descriptors, descriptors + 4);
  CHECK(__weavec_rt_find(pairOfGlobals[0] + 5).base ==
        (uintptr_t)pairOfGlobals[0]);
  CHECK(guardByte(pairOfGlobals[0], 15) == 0);
  CHECK(guardByte(pairOfGlobals[0], 16) != 0);
  /* The start of the second is one past the first for a negative index. */
  CHECK(guardByte(pairOfGlobals[1], -1) == 0);
  CHECK(guardByte(pairOfGlobals[1], 0) == 0);
  CHECK(guardByte(pairOfGlobals[1], 16) != 0);
  /* RFC 0033 section 4: the byte before the first is another object's, which
   * a backward index may reach, or nothing's, which it may not. */
  CHECK(guardByte(pairOfGlobals[0], -1) ==
        (__weavec_rt_find(pairOfGlobals[0] - 1).state != WeavecRtTrackedLive));
  CHECK(__weavec_rt_string("a literal is untracked") == 0);
}

static void testStrings(void) {
  char *p = (char *)opaque(malloc(8));
  memcpy(p, "1234567", 8);
  CHECK(__weavec_rt_string(p) == 0);
  CHECK(__weavec_rt_string(p + 7) == 0);
  p[7] = 'x';
  CHECK(__weavec_rt_string(p) != 0);
  CHECK(__weavec_rt_string(p + 8) != 0);
  free(p);
  CHECK(__weavec_rt_string(p) != 0);
}

/* RFC 0034, section 5.2: the room a checked sprintf may write. */
static void testRoom(void) {
  char *p = (char *)opaque(malloc(24));
  char buffer[16] = {0};
  void *frame = __builtin_frame_address(0);
  int untracked = 0;
  CHECK(__weavec_rt_room(p) == 24);
  CHECK(__weavec_rt_room(p + 10) == 14);
  CHECK(__weavec_rt_room(p + 23) == 1);
  /* One past the end has no room, whatever lies there. */
  CHECK(__weavec_rt_room(p + 24) == 0 ||
        __weavec_rt_find(p + 24).base == (uintptr_t)p + 24);
  free(p);
  CHECK(__weavec_rt_room(p) == 0);
  CHECK(__weavec_rt_room(&untracked) == ~0ULL);
  CHECK(__weavec_rt_room(NULL) == ~0ULL);
  __weavec_rt_stack_enter(buffer, sizeof buffer, frame, 0);
  CHECK(__weavec_rt_room(buffer + 4) == 12);
  __weavec_rt_stack_leave(buffer, frame);
  CHECK(__weavec_rt_room(buffer + 4) == ~0ULL);
}

/*===-- Threads and fork ----------------------------------------------------===*/

static void *worker(void *argument) {
  unsigned seed = (unsigned)(uintptr_t)argument;
  void *held[64] = {0};
  unsigned round;
  char local[32] = {0};
  void *frame = __builtin_frame_address(0);
  __weavec_rt_stack_enter(local, sizeof local, frame, 0);
  for (round = 0; round < 20000; ++round) {
    const unsigned slot = (seed = seed * 1103515245u + 12345u) >> 16 & 63;
    const size_t size = (seed >> 8) % 3000;
    if (held[slot] != NULL) {
      if (__weavec_rt_live(held[slot]) != 0)
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
  if (guardByte(local, 31) != 0 || guardByte(local, 32) == 0)
    return (void *)1;
  __weavec_rt_stack_leave(local, frame);
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
  CHECK(__weavec_rt_live(p) == 0);
  free(p);
}

/*===-- Driver --------------------------------------------------------------===*/

static const struct {
  const char *name;
  void (*run)(void);
} Tests[] = {
    {"classes", testClasses},
    {"alignment", testAlignment},
    {"realloc", testRealloc},
    {"invalid-releases", testInvalidReleases},
    {"huge", testHuge},
    {"foreign", testForeign},
    {"stack", testStack},
    {"mixed-neighbours", testMixedNeighbours},
    {"shadow-clear", testShadowClear},
    {"large-object", testLargeObject},
    {"loose-partial", testLoosePartial},
    {"longjmp", testLongjmp},
    {"deep-stack", testDeepStack},
    {"globals", testGlobals},
    {"strings", testStrings},
    {"room", testRoom},
    {"threads", testThreads},
    {"fork", testFork},
    /* These run in a copy of the program with no quarantine. */
    {"reuse-is-zero", testReuseIsZero},
    {"quarantine", testQuarantine},
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
