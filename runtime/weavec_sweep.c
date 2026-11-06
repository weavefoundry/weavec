/*===- weavec_sweep.c - Reuse after a sweep -------------------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0036, section 3.4. A released heap block waits in its class's
|* quarantine; a sweep moves it to the free list only when no pointer to it
|* is left:
|*
|*   1. stop every other thread (Darwin: thread_suspend; Linux: a signal
|*      whose handler waits), holding every allocator lock;
|*   2. mark, scanning conservatively word by aligned word: each thread's
|*      registers (vector ones too), stack and thread-specific data, every
|*      readable mapping of the process but the runtime's own, code and
|*      shared files the program cannot write (of the dyld shared cache,
|*      only the writable segments of the images the process loaded), and
|*      every live heap block (RFC 0037 section 1);
|*   3. recycle what nothing marked, and resume.
|*
|* A sweep that cannot stop every thread, or that finds a thread it did not
|* stop, recycles nothing: safety never depends on it running.
|*
|* The sweep runs on the thread whose release (or allocation) found the
|* quarantine over budget. It calls nothing that takes a lock another
|* thread could hold while stopped: no allocation, no stdio, no dyld.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>
#else
#include <sys/syscall.h>
#include <linux/futex.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

/*===-- Internal mappings ---------------------------------------------------===*/

/* The runtime's own mappings, which hold no program pointer (the shadow,
 * the arena's reservation and metadata, the huge table, the marks). */
enum { InternalSlots = 512 };
static struct {
  uintptr_t low;
  uintptr_t high;
} internalRanges[InternalSlots];
static unsigned internalCount;
static WeavecRtLock internalLock;

void weavecRtInternal(uintptr_t low, uintptr_t high) {
  weavecRtLock(&internalLock);
  if (internalCount < InternalSlots) {
    internalRanges[internalCount].low = low;
    internalRanges[internalCount].high = high;
    ++internalCount;
  }
  weavecRtUnlock(&internalLock);
}

void weavecRtInternalDrop(uintptr_t low) {
  unsigned i;
  weavecRtLock(&internalLock);
  for (i = 0; i < internalCount; ++i)
    if (internalRanges[i].low == low) {
      internalRanges[i] = internalRanges[--internalCount];
      break;
    }
  weavecRtUnlock(&internalLock);
}

/*===-- The stopped world ---------------------------------------------------===*/

/* A stretch of memory to scan, and the stacks: a thread's stack is scanned
 * from its stack pointer (less the red zone below it) to the end of the
 * mapping that holds it; below it only when the mapping is not a thread's
 * own stack (RFC 0037 section 1). */
enum { MaxThreads = 4096 };
/* Words of a stopped thread's registers: general and vector (Darwin). */
enum { Registers = 128 };
#if defined(__x86_64__)
enum { RedZone = 128 };
#else
enum { RedZone = 0 };
#endif

typedef struct {
  /* Two stack pointers for each stopped thread on Linux (the handler's and
   * the interrupted code's) and the sweeping thread's. */
  uintptr_t stacks[2 * MaxThreads + 1];
  unsigned stackCount;
  /* Registers of the stopped threads (Darwin), scanned as words. */
  uintptr_t registers[MaxThreads][Registers];
  unsigned registerCount;
  /* Each thread's thread-specific data (Darwin: its TSD array). */
  uintptr_t tsd[MaxThreads + 1];
  unsigned tsdCount;
} World;

/* The sweep's state, mapped once: too large for a stack. */
static World *world;
static WeavecRtLock sweepLock;

void weavecRtSweepLock(void) { weavecRtLock(&sweepLock); }
void weavecRtSweepUnlock(void) { weavecRtUnlock(&sweepLock); }

static int trySweepLock(void) {
  return __atomic_exchange_n(&sweepLock.held, 1, __ATOMIC_ACQUIRE) == 0;
}

static uint64_t nanoseconds(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

/* Sorts the internal ranges by their start, for scanExcluding. */
static void sortInternal(void) {
  unsigned i;
  for (i = 1; i < internalCount; ++i) {
    const uintptr_t low = internalRanges[i].low;
    const uintptr_t high = internalRanges[i].high;
    unsigned j = i;
    while (j > 0 && internalRanges[j - 1].low > low) {
      internalRanges[j] = internalRanges[j - 1];
      --j;
    }
    internalRanges[j].low = low;
    internalRanges[j].high = high;
  }
}

/*===-- Mark workers (RFC 0037 section 3) ----------------------------------===*/

/* The mark reads every root and every live heap block once, with no
 * tracing, so it divides without coordination: a large sweep lists its
 * ranges (words of memory, or runs of a region's slots) first, then the
 * sweeping thread and the workers take them one by one. Workers are
 * threads of the runtime's, on stacks of its own, never stopped, with every
 * signal blocked; they call nothing that takes a lock a stopped thread may
 * hold. */
enum {
  MaxWorkers = 7,
  WorkerStack = 256 * 1024,
  MarkChunk = 4 << 20,
  ParallelBytes = 64 << 20,
  WorkSlots = 1 << 16,
  WordsWork = 0xFFFFFFFFu
};

typedef struct {
  uintptr_t low;
  uintptr_t high;
  /* WordsWork: the words of [low, high); else the slots [low, high) of this
   * region. */
  uint32_t region;
} Work;

static Work *work;
static unsigned workCount;
static unsigned workNext;
static int collecting;
static unsigned workersWanted;
static unsigned workerCount;
static int workersFailed;
#if defined(__APPLE__)
static mach_port_t workerThreads[MaxWorkers];
#else
static volatile pid_t workerThreads[MaxWorkers];
#endif
static pthread_mutex_t workLock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t workGo = PTHREAD_COND_INITIALIZER;
static pthread_cond_t workDone = PTHREAD_COND_INITIALIZER;
static unsigned workRound;
static unsigned workersFinished;

__attribute__((constructor)) static void readSweepThreads(void) {
  const char *text = getenv("WEAVEC_RT_SWEEP_THREADS");
  const long cores = sysconf(_SC_NPROCESSORS_ONLN);
  workersWanted = cores > 1 ? (unsigned)(cores - 1) : 0;
  if (text != NULL && *text >= '0' && *text <= '9')
    workersWanted = (unsigned)atoi(text);
  if (workersWanted > MaxWorkers)
    workersWanted = MaxWorkers;
}

static void markWork(const Work *item) {
  if (item->region == WordsWork)
    weavecRtMarkWords(item->low, item->high);
  else
    weavecRtMarkSlots(item->region, (uint32_t)item->low,
                      (uint32_t)item->high);
}

/* Lists an item, or marks it at once when the list is full. */
static void addWork(uintptr_t low, uintptr_t high, uint32_t region) {
  if (workCount < WorkSlots) {
    work[workCount].low = low;
    work[workCount].high = high;
    work[workCount].region = region;
    ++workCount;
  } else {
    const Work item = {low, high, region};
    markWork(&item);
  }
}

static void addSlots(unsigned region, uint32_t first, uint32_t last) {
  addWork(first, last, region);
}

/* Marks from the words of [low, high): now, or listed in chunks while a
 * parallel sweep collects its ranges. */
static void markRange(uintptr_t low, uintptr_t high) {
  if (!collecting) {
    weavecRtMarkWords(low, high);
    return;
  }
  while (high - low > MarkChunk) {
    addWork(low, low + MarkChunk, WordsWork);
    low += MarkChunk;
  }
  if (low < high)
    addWork(low, high, WordsWork);
}

static void takeWork(void) {
  for (;;) {
    const unsigned i = __atomic_fetch_add(&workNext, 1, __ATOMIC_RELAXED);
    if (i >= workCount)
      return;
    markWork(&work[i]);
  }
}

static void *markWorker(void *argument) {
  unsigned seen = 0;
#if defined(__APPLE__)
  (void)argument;
#else
  workerThreads[(uintptr_t)argument] = (pid_t)syscall(SYS_gettid);
#endif
  (void)pthread_mutex_lock(&workLock);
  for (;;) {
    while (workRound == seen)
      (void)pthread_cond_wait(&workGo, &workLock);
    seen = workRound;
    (void)pthread_mutex_unlock(&workLock);
    takeWork();
    (void)pthread_mutex_lock(&workLock);
    ++workersFinished;
    (void)pthread_cond_signal(&workDone);
  }
  return NULL;
}

/* Starts the workers a sweep of a large heap wants, before it stops the
 * world (signals are blocked, so the workers start with them blocked). */
static void startWorkers(void) {
  while (!workersFailed && workerCount < workersWanted &&
         weavecRtLiveBytes() >= ParallelBytes) {
    pthread_attr_t attributes;
    pthread_t thread;
    void *stack = mmap(NULL, WorkerStack, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int ok = stack != MAP_FAILED && pthread_attr_init(&attributes) == 0;
    if (ok) {
      ok = pthread_attr_setstack(&attributes, stack, WorkerStack) == 0 &&
           pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED) ==
               0;
      /* Its stack holds no program pointer. */
      if (ok)
        weavecRtInternal((uintptr_t)stack, (uintptr_t)stack + WorkerStack);
#if !defined(__APPLE__)
      workerThreads[workerCount] = 0;
#endif
      ok = ok && pthread_create(&thread, &attributes, markWorker,
                                (void *)(uintptr_t)workerCount) == 0;
      (void)pthread_attr_destroy(&attributes);
    }
    if (!ok) {
      workersFailed = 1;
      return;
    }
#if defined(__APPLE__)
    workerThreads[workerCount] = pthread_mach_thread_np(thread);
#else
    while (__atomic_load_n(&workerThreads[workerCount], __ATOMIC_ACQUIRE) == 0)
      sched_yield();
#endif
    ++workerCount;
  }
}

/* Marks the listed work on the sweeping thread and every worker. */
static void runWork(void) {
  (void)pthread_mutex_lock(&workLock);
  workNext = 0;
  workersFinished = 0;
  ++workRound;
  (void)pthread_cond_broadcast(&workGo);
  (void)pthread_mutex_unlock(&workLock);
  takeWork();
  (void)pthread_mutex_lock(&workLock);
  while (workersFinished < workerCount)
    (void)pthread_cond_wait(&workDone, &workLock);
  (void)pthread_mutex_unlock(&workLock);
}

#if defined(__APPLE__)
static int isWorker(mach_port_t thread) {
#else
static int isWorker(pid_t thread) {
#endif
  unsigned i;
  for (i = 0; i < workerCount; ++i)
    if (workerThreads[i] == thread)
      return 1;
  return 0;
}

void weavecRtSweepForkChild(void) {
  workerCount = 0;
  workRound = 0;
  workersFinished = 0;
  (void)pthread_mutex_init(&workLock, NULL);
  (void)pthread_cond_init(&workGo, NULL);
  (void)pthread_cond_init(&workDone, NULL);
}

/* Scans [low, high) but the runtime's own mappings in it (sorted). */
static void scanExcluding(uintptr_t low, uintptr_t high) {
  unsigned i;
  for (i = 0; i < internalCount && low < high; ++i) {
    const uintptr_t a = internalRanges[i].low;
    const uintptr_t b = internalRanges[i].high;
    if (b <= low || a >= high)
      continue;
    if (a > low)
      markRange(low, a);
    low = b;
  }
  if (low < high)
    markRange(low, high);
}

/* The lowest stack pointer of a stopped thread inside [low, high), or 0. */
static uintptr_t stackIn(uintptr_t low, uintptr_t high) {
  uintptr_t sp = 0;
  unsigned i;
  for (i = 0; i < world->stackCount; ++i)
    if (world->stacks[i] >= low && world->stacks[i] < high &&
        (sp == 0 || world->stacks[i] < sp))
      sp = world->stacks[i];
  return sp;
}

/* Scans a thread's own stack mapping from its lowest stack pointer: the
 * bytes below it are dead. Threads whose stacks share one mapping are all
 * scanned from it. */
static void scanStack(uintptr_t low, uintptr_t high) {
  const uintptr_t sp = stackIn(low, high);
  if (sp == 0)
    scanExcluding(low, high);
  else
    scanExcluding(sp - RedZone > low ? sp - RedZone : low, high);
}

/* The first address at or after `address` outside the runtime's own
 * mappings (sorted). */
static uintptr_t skipInternal(uintptr_t address) {
  unsigned i;
  for (i = 0; i < internalCount; ++i)
    if (address >= internalRanges[i].low && address < internalRanges[i].high)
      address = internalRanges[i].high;
  return address;
}

#if defined(__APPLE__)

/*===-- Darwin --------------------------------------------------------------===*/

static thread_act_array_t threads;
static mach_msg_type_number_t threadCount;
/* The threads that terminated before they could be suspended: their port
 * is dead (a thread that pthread_join has seen exit may still be listed).
 * Nothing of theirs is a root, and their names stay held until the resume,
 * so that no new thread takes one. */
static unsigned char gone[MaxThreads];

static int stopThreads(void) {
  const thread_t self = mach_thread_self();
  mach_msg_type_number_t i;
  int ok = 1;
  (void)mach_port_deallocate(mach_task_self(), self);
  if (task_threads(mach_task_self(), &threads, &threadCount) != KERN_SUCCESS) {
    threads = NULL;
    threadCount = 0;
    return 0;
  }
  if (threadCount > MaxThreads)
    ok = 0;
  for (i = 0; i < threadCount && ok; ++i) {
    gone[i] = 0;
    if (threads[i] == self || isWorker(threads[i]))
      continue;
    if (thread_suspend(threads[i]) != KERN_SUCCESS) {
      thread_basic_info_data_t basic;
      mach_msg_type_number_t count = THREAD_BASIC_INFO_COUNT;
      /* A thread terminated; anything else (a thread that cannot be
       * suspended) holds up recycling. */
      gone[i] = thread_info(threads[i], THREAD_BASIC_INFO,
                            (thread_info_t)&basic, &count) != KERN_SUCCESS;
      ok = gone[i];
    }
  }
  /* The threads not suspended are not resumed either. */
  for (; i < threadCount; ++i)
    if (threads[i] != self && !isWorker(threads[i])) {
      (void)mach_port_deallocate(mach_task_self(), threads[i]);
      threads[i] = MACH_PORT_NULL;
    }
  if (!ok)
    return 0;
  /* Each thread's thread-specific data, the sweeping thread's too. */
  for (i = 0; i < threadCount; ++i) {
    thread_identifier_info_data_t identity;
    mach_msg_type_number_t count = THREAD_IDENTIFIER_INFO_COUNT;
    if (threads[i] == MACH_PORT_NULL || isWorker(threads[i]) ||
        gone[i])
      continue;
    if (thread_info(threads[i], THREAD_IDENTIFIER_INFO,
                    (thread_info_t)&identity, &count) != KERN_SUCCESS)
      return 0;
    world->tsd[world->tsdCount++] = (uintptr_t)identity.thread_handle;
  }
  /* Each stopped thread's registers, general and vector, and stack
   * pointer: a pointer may live only in a vector register (RFC 0037
   * section 1). */
  for (i = 0; i < threadCount; ++i) {
    uintptr_t *words = world->registers[world->registerCount];
    if (threads[i] == self || threads[i] == MACH_PORT_NULL ||
        isWorker(threads[i]) || gone[i])
      continue;
#if defined(__aarch64__)
    {
      arm_thread_state64_t state;
      arm_neon_state64_t vector;
      mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
      unsigned r;
      if (thread_get_state(threads[i], ARM_THREAD_STATE64,
                           (thread_state_t)&state, &count) != KERN_SUCCESS)
        return 0;
      for (r = 0; r < 29; ++r)
        words[r] = state.__x[r];
      words[29] = (uintptr_t)arm_thread_state64_get_fp(state);
      words[30] = (uintptr_t)arm_thread_state64_get_lr(state);
      world->stacks[world->stackCount] =
          (uintptr_t)arm_thread_state64_get_sp(state);
      count = ARM_NEON_STATE64_COUNT;
      if (thread_get_state(threads[i], ARM_NEON_STATE64,
                           (thread_state_t)&vector, &count) != KERN_SUCCESS)
        return 0;
      memcpy(words + 31, vector.__v, sizeof vector.__v);
    }
#elif defined(__x86_64__)
    {
      x86_thread_state64_t state;
      x86_avx_state64_t vector;
      mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
      const size_t general = sizeof state / sizeof(uintptr_t);
      if (thread_get_state(threads[i], x86_THREAD_STATE64,
                           (thread_state_t)&state, &count) != KERN_SUCCESS)
        return 0;
      memcpy(words, &state, general * sizeof(uintptr_t));
      world->stacks[world->stackCount] = state.__rsp;
      count = x86_AVX_STATE64_COUNT;
      if (thread_get_state(threads[i], x86_AVX_STATE64,
                           (thread_state_t)&vector, &count) != KERN_SUCCESS) {
        /* No AVX: the SSE registers. */
        x86_float_state64_t sse;
        count = x86_FLOAT_STATE64_COUNT;
        if (thread_get_state(threads[i], x86_FLOAT_STATE64,
                             (thread_state_t)&sse, &count) != KERN_SUCCESS)
          return 0;
        memcpy(words + general, &sse,
               sizeof sse < (Registers - general) * sizeof(uintptr_t)
                   ? sizeof sse
                   : (Registers - general) * sizeof(uintptr_t));
      } else {
        memcpy(words + general, &vector,
               sizeof vector < (Registers - general) * sizeof(uintptr_t)
                   ? sizeof vector
                   : (Registers - general) * sizeof(uintptr_t));
      }
    }
#else
    return 0;
#endif
    ++world->registerCount;
    ++world->stackCount;
  }
  return 1;
}

static void resumeThreads(void) {
  const thread_t self = mach_thread_self();
  mach_msg_type_number_t i;
  (void)mach_port_deallocate(mach_task_self(), self);
  for (i = 0; i < threadCount; ++i) {
    if (threads[i] == MACH_PORT_NULL)
      continue;
    if (threads[i] != self && !isWorker(threads[i]) && !gone[i])
      (void)thread_resume(threads[i]);
    (void)mach_port_deallocate(mach_task_self(), threads[i]);
  }
  (void)vm_deallocate(mach_task_self(), (vm_address_t)threads,
                      threadCount * sizeof(thread_t));
  threads = NULL;
  threadCount = 0;
}

/* Whether a thread exists that was not stopped, compared by identity (a
 * thread that started while another exited keeps the count); one that
 * exited since is no root. */
static int threadsChanged(void) {
  thread_act_array_t now;
  mach_msg_type_number_t count;
  mach_msg_type_number_t i;
  int changed = 0;
  if (task_threads(mach_task_self(), &now, &count) != KERN_SUCCESS)
    return 1;
  for (i = 0; i < count && !changed; ++i) {
    mach_msg_type_number_t j = 0;
    if (!MACH_PORT_VALID(now[i]))
      continue;
    while (j < threadCount && threads[j] != now[i])
      ++j;
    changed = j == threadCount;
  }
  for (i = 0; i < count; ++i)
    (void)mach_port_deallocate(mach_task_self(), now[i]);
  (void)vm_deallocate(mach_task_self(), (vm_address_t)now,
                      count * sizeof(thread_t));
  return changed;
}

/* The pages of [low, high) the program may have written (mincore); a page
 * never touched, or a private file page never modified, holds nothing it
 * wrote. With `resident`, every resident page: a shared file's page that
 * msync wrote back is clean but holds what the program stored. */
static void scanResident(uintptr_t low, uintptr_t high, int resident) {
  enum { Pages = 4096 };
  const uintptr_t page = (uintptr_t)getpagesize();
  char vector[Pages];
  while (low < high) {
    const uintptr_t chunk =
        high - low > Pages * page ? Pages * page : high - low;
    const uintptr_t count = (chunk + page - 1) / page;
    uintptr_t i;
    uintptr_t run = 0;
    if (mincore((void *)low, (size_t)chunk, vector) != 0) {
      scanExcluding(low, low + chunk);
      low += chunk;
      continue;
    }
    /* Runs of touched pages, one scan each. */
    for (i = 0; i <= count; ++i) {
      /* A page the program may have written: modified, compressed or
       * paged out, or anonymous; not a clean page of a file (the shared
       * cache's data as dyld mapped it). */
      const int touched =
          i < count &&
          ((vector[i] & (MINCORE_MODIFIED | MINCORE_PAGED_OUT)) != 0 ||
           (vector[i] & (MINCORE_INCORE | MINCORE_ANONYMOUS)) ==
               (MINCORE_INCORE | MINCORE_ANONYMOUS) ||
           (resident && (vector[i] & MINCORE_INCORE) != 0));
      if (touched)
        continue;
      if (i > run) {
        const uintptr_t end = low + i * page < low + chunk ? low + i * page
                                                           : low + chunk;
        scanExcluding(low + run * page, end);
      }
      run = i + 1;
    }
    low += chunk;
  }
}

#ifndef VM_MEMORY_VM_RECLAIM
#define VM_MEMORY_VM_RECLAIM 22
#endif

enum { ScanNone, ScanTouched, ScanResident };

/* Which pages of a region may hold what the program wrote (RFC 0037
 * section 1): none of code it cannot write or of the kernel's reclaim ring
 * (the system allocator's free ranges); every resident page of a file
 * mapping it can write (a shared file's page that msync wrote back is
 * clean; the kernel reports a file mapped once as private, so a private
 * one is read so too); otherwise, whatever the protection now (memory made
 * read-only after it was written), the pages written or paged out (an
 * untouched page reads zero, a clean page of a file holds the file, and a
 * shared file mapping the program cannot write through has none). The
 * page counts cost one call per region; mincore then finds the pages, at
 * about a microsecond each. */
static int scanKind(const vm_region_submap_info_data_64_t *info) {
  const int writable = (info->protection & VM_PROT_WRITE) != 0;
  if ((info->protection & VM_PROT_READ) == 0 ||
      ((info->protection & VM_PROT_EXECUTE) != 0 && !writable) ||
      info->share_mode == SM_EMPTY || info->user_tag == VM_MEMORY_VM_RECLAIM)
    return ScanNone;
  if (info->external_pager && writable && info->pages_resident != 0)
    return ScanResident;
  return info->pages_dirtied != 0 || info->pages_swapped_out != 0 ? ScanTouched
                                                                   : ScanNone;
}

/* RFC 0036 section 3.4: the writable segments of the images the process
 * loaded from the dyld shared cache, kept from dyld's callbacks. The sweep
 * scans them instead of the cache's writable mappings, which hold the data
 * of every image in the cache (hundreds of megabytes the process never
 * touched, whose pages mincore would be asked about on every sweep). */
#ifndef MH_DYLIB_IN_CACHE
#define MH_DYLIB_IN_CACHE 0x80000000
#endif
extern const void *_dyld_get_shared_cache_range(size_t *length);

enum { ImageSegmentSlots = 4096 };
static struct {
  const struct mach_header *header;
  uintptr_t low;
  uintptr_t high;
} imageSegments[ImageSegmentSlots];
static unsigned imageSegmentCount;
/* Taken by the callbacks and by the sweep before it stops the threads, so
 * that no stopped thread holds it. */
static WeavecRtLock imageLock;
static uintptr_t cacheLow;
static uintptr_t cacheHigh;

static void addImage(const struct mach_header *header, intptr_t slide) {
  const struct mach_header_64 *image = (const struct mach_header_64 *)header;
  const struct load_command *command;
  uint32_t i;
  if (image->magic != MH_MAGIC_64 || (image->flags & MH_DYLIB_IN_CACHE) == 0)
    return;
  command = (const struct load_command *)(image + 1);
  weavecRtLock(&imageLock);
  for (i = 0; i < image->ncmds; ++i) {
    if (command->cmd == LC_SEGMENT_64) {
      const struct segment_command_64 *segment =
          (const struct segment_command_64 *)command;
      if ((segment->initprot & VM_PROT_WRITE) != 0 && segment->vmsize != 0 &&
          imageSegmentCount < ImageSegmentSlots) {
        imageSegments[imageSegmentCount].header = header;
        imageSegments[imageSegmentCount].low =
            (uintptr_t)(segment->vmaddr + (uint64_t)slide);
        imageSegments[imageSegmentCount].high =
            (uintptr_t)(segment->vmaddr + (uint64_t)slide + segment->vmsize);
        ++imageSegmentCount;
      }
    }
    command = (const struct load_command *)((const char *)command +
                                            command->cmdsize);
  }
  weavecRtUnlock(&imageLock);
}

static void removeImage(const struct mach_header *header, intptr_t slide) {
  unsigned i = 0;
  (void)slide;
  weavecRtLock(&imageLock);
  while (i < imageSegmentCount)
    if (imageSegments[i].header == header)
      imageSegments[i] = imageSegments[--imageSegmentCount];
    else
      ++i;
  weavecRtUnlock(&imageLock);
}

/* dyld calls addImage at once for every image already loaded. */
__attribute__((constructor)) static void watchImages(void) {
  size_t length = 0;
  const void *start = _dyld_get_shared_cache_range(&length);
  if (start == NULL || length == 0)
    return;
  cacheLow = (uintptr_t)start;
  cacheHigh = cacheLow + length;
  _dyld_register_func_for_add_image(addImage);
  _dyld_register_func_for_remove_image(removeImage);
}

static void lockImages(void) { weavecRtLock(&imageLock); }
static void unlockImages(void) { weavecRtUnlock(&imageLock); }

/* Every readable, writable mapping that was touched, but the shared
 * cache's, whose loaded images' writable segments are scanned instead;
 * whether the walk reached the end of the address space. */
static int scanMappings(void) {
  mach_vm_address_t address = 0;
  unsigned i;
  for (;;) {
    kern_return_t result;
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    /* The runtime's own reservations (the arena's committed chunks are a
     * region each) are skipped whole, not asked about one by one. */
    address = skipInternal(address);
    if (address >= cacheLow && address < cacheHigh)
      address = cacheHigh;
    result = mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
                                    (vm_region_recurse_info_t)&info, &count);
    if (result == KERN_INVALID_ADDRESS)
      break;
    if (result != KERN_SUCCESS)
      return 0;
    if (!info.is_submap && (address + size <= cacheLow || address >= cacheHigh)) {
      const int kind = scanKind(&info);
      const uintptr_t low = (uintptr_t)address;
      const uintptr_t high = (uintptr_t)(address + size);
      /* A thread's own stack (libpthread and the kernel tag it) is
       * resident from its stack pointer up, and dead below; any other
       * mapping holding a stack pointer (a coroutine's, a fiber pool's, a
       * stack the program gave pthread_attr_setstack) holds other stacks
       * below it. */
      if (kind != ScanNone && info.user_tag == VM_MEMORY_STACK &&
          stackIn(low, high) != 0)
        scanStack(low, high);
      else if (kind != ScanNone)
        scanResident(low, high, kind == ScanResident);
    }
    address += size;
  }
  for (i = 0; i < imageSegmentCount; ++i)
    scanResident(imageSegments[i].low, imageSegments[i].high, 0);
  /* Thread-specific data outside the stacks and the images' segments (the
   * main thread's lies in libpthread's data in the shared cache): each
   * thread's TSD array, up to the end of the region that holds it. */
  for (i = 0; i < world->tsdCount; ++i) {
    enum { TsdSlots = 1024 };
    mach_vm_address_t at = world->tsd[i];
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    uintptr_t high;
    if (world->tsd[i] < cacheLow || world->tsd[i] >= cacheHigh)
      continue;
    while (mach_vm_region_recurse(mach_task_self(), &at, &size, &depth,
                                  (vm_region_recurse_info_t)&info,
                                  &count) == KERN_SUCCESS &&
           info.is_submap) {
      ++depth;
      at = world->tsd[i];
      count = VM_REGION_SUBMAP_INFO_COUNT_64;
    }
    if (at > world->tsd[i] || (info.protection & VM_PROT_READ) == 0)
      continue;
    high = world->tsd[i] + TsdSlots * sizeof(uintptr_t);
    if (high > (uintptr_t)(at + size))
      high = (uintptr_t)(at + size);
    weavecRtMarkWords(world->tsd[i], high);
  }
  return 1;
}

#else

/*===-- Linux ---------------------------------------------------------------===*/

static int stopSignal;
static pid_t stopped[MaxThreads];
static volatile uintptr_t stoppedSp[MaxThreads];
static volatile uintptr_t stoppedInterruptedSp[MaxThreads];
static unsigned stopTargets;
static int stoppedCount;
static int stopGeneration;
/* Whether a stop is in progress: a signal delivered after the sweep resumed
 * the threads is answered at once. */
static int stopping;

static long futex(int *address, int op, int value,
                  const struct timespec *timeout) {
  return syscall(SYS_futex, address, op, value, timeout, NULL, 0);
}

static void stopHandler(int signal, siginfo_t *info, void *context) {
  const int saved = errno;
  const pid_t self = (pid_t)syscall(SYS_gettid);
  const int generation = __atomic_load_n(&stopGeneration, __ATOMIC_ACQUIRE);
  volatile uintptr_t here = 0;
  unsigned i;
  (void)signal;
  (void)info;
  if (!__atomic_load_n(&stopping, __ATOMIC_ACQUIRE))
    return;
  __builtin_unwind_init();
  for (i = 0; i < stopTargets && stopped[i] != self; ++i)
    ;
  /* Only the threads asked count: not the sweeper, nor one sent it by kill. */
  if (i == stopTargets)
    return;
  stoppedSp[i] = (uintptr_t)&here;
#if defined(__x86_64__)
  stoppedInterruptedSp[i] =
      (uintptr_t)((const ucontext_t *)context)->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
  stoppedInterruptedSp[i] =
      (uintptr_t)((const ucontext_t *)context)->uc_mcontext.sp;
#else
  (void)context;
#endif
  __atomic_add_fetch(&stoppedCount, 1, __ATOMIC_RELEASE);
  (void)futex(&stoppedCount, FUTEX_WAKE_PRIVATE, 1, NULL);
  while (__atomic_load_n(&stopGeneration, __ATOMIC_ACQUIRE) == generation)
    (void)futex(&stopGeneration, FUTEX_WAIT_PRIVATE, generation, NULL);
  errno = saved;
}

/* The signal that stops threads: WEAVEC_RT_SWEEP_SIGNAL, SIGPWR, or SIGXCPU
 * when the program handles SIGPWR; 0 when none is free. */
static void chooseSignal(void) {
  static const int Candidates[2] = {SIGPWR, SIGXCPU};
  const char *text = getenv("WEAVEC_RT_SWEEP_SIGNAL");
  const int asked = text != NULL ? atoi(text) : 0;
  int i;
  for (i = asked != 0 ? -1 : 0; i < 2; ++i) {
    const int signal = i < 0 ? asked : Candidates[i];
    struct sigaction old;
    struct sigaction action;
    if (sigaction(signal, NULL, &old) != 0)
      continue;
    if (i >= 0 && ((old.sa_flags & SA_SIGINFO) != 0 ||
                   (old.sa_handler != SIG_DFL && old.sa_handler != SIG_IGN)))
      continue;
    memset(&action, 0, sizeof action);
    action.sa_sigaction = stopHandler;
    action.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&action.sa_mask);
    if (sigaction(signal, &action, NULL) == 0) {
      const uint64_t bit = (uint64_t)1 << (signal - 1);
      stopSignal = signal;
      (void)syscall(SYS_rt_sigprocmask, SIG_UNBLOCK, &bit, NULL, sizeof bit);
      return;
    }
  }
}

static int installHandler(void) {
  static pthread_once_t once = PTHREAD_ONCE_INIT;
  (void)pthread_once(&once, chooseSignal);
  return stopSignal != 0;
}

/* RFC 0038, amendment 13: masks the program sets leave out the sweep's
 * signal (chosen at the first, before a thread can block it), and the C
 * library's two internal ones as its own functions do. */
static int setMask(int how, const sigset_t *set, sigset_t *old) {
  uint64_t mask = 0;
  if (set != NULL && how != SIG_UNBLOCK && installHandler())
    mask = (uint64_t)1 << (stopSignal - 1);
  if (set != NULL && how != SIG_UNBLOCK) {
    mask = ~(mask | (uint64_t)3 << 31) & *(const uint64_t *)(const void *)set;
    set = (const sigset_t *)(const void *)&mask;
  }
  return (int)syscall(SYS_rt_sigprocmask, how, set, old, sizeof mask);
}

WEAVEC_RT_REPLACEABLE int sigprocmask(int how, const sigset_t *set,
                                      sigset_t *old) {
  return setMask(how, set, old);
}

WEAVEC_RT_REPLACEABLE int pthread_sigmask(int how, const sigset_t *set,
                                          sigset_t *old) {
  return setMask(how, set, old) != 0 ? errno : 0;
}

/* The tasks of the process, by reading /proc/self/task with getdents64
 * (opendir allocates). */
static unsigned listTasks(pid_t *out, unsigned max, int *overflow) {
  char buffer[4096];
  unsigned count = 0;
  const int fd = open("/proc/self/task", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  *overflow = 0;
  if (fd < 0)
    return 0;
  for (;;) {
    const long got = syscall(SYS_getdents64, fd, buffer, sizeof buffer);
    long at = 0;
    if (got <= 0)
      break;
    while (at < got) {
      const unsigned short length = *(unsigned short *)(buffer + at + 16);
      const char *name = buffer + at + 19;
      if (name[0] >= '0' && name[0] <= '9') {
        if (count < max)
          out[count++] = (pid_t)atoi(name);
        else
          *overflow = 1;
      }
      at += length;
    }
  }
  (void)close(fd);
  return count;
}

static pid_t tasks[MaxThreads];
static unsigned taskCount;

static int stopThreads(void) {
  const pid_t self = (pid_t)syscall(SYS_gettid);
  const pid_t process = getpid();
  int overflow;
  unsigned i;
  unsigned sent = 0;
  uint64_t deadline;
  taskCount = listTasks(tasks, MaxThreads, &overflow);
  if (overflow || taskCount == 0)
    return 0;
  if (taskCount == 1 + workerCount)
    return 1;
  if (!installHandler())
    return 0;
  stopTargets = 0;
  for (i = 0; i < taskCount; ++i)
    if (tasks[i] != self && !isWorker(tasks[i])) {
      stoppedSp[stopTargets] = 0;
      stoppedInterruptedSp[stopTargets] = 0;
      stopped[stopTargets++] = tasks[i];
    }
  __atomic_store_n(&stoppedCount, 0, __ATOMIC_RELEASE);
  __atomic_store_n(&stopping, 1, __ATOMIC_RELEASE);
  for (i = 0; i < stopTargets; ++i) {
    if (syscall(SYS_tgkill, process, stopped[i], stopSignal) == 0)
      ++sent;
    else
      stopped[i] = 0;
  }
  /* A thread that does not answer within a second (it blocks the signal)
   * holds up recycling; one that has exited (pthread_join may return before
   * it leaves the list, and an exiting thread takes no signal) is no root. */
  deadline = nanoseconds() + 1000000000ULL;
  for (;;) {
    const int now = __atomic_load_n(&stoppedCount, __ATOMIC_ACQUIRE);
    struct timespec wait = {0, 1000000};
    if ((unsigned)now >= sent)
      break;
    if (nanoseconds() > deadline)
      return 0;
    (void)futex(&stoppedCount, FUTEX_WAIT_PRIVATE, now, &wait);
    for (i = 0; i < stopTargets; ++i)
      if (stopped[i] != 0 &&
          __atomic_load_n(&stoppedSp[i], __ATOMIC_ACQUIRE) == 0 &&
          syscall(SYS_tgkill, process, stopped[i], 0) != 0 && errno == ESRCH) {
        stopped[i] = 0;
        --sent;
      }
  }
  for (i = 0; i < stopTargets; ++i) {
    if (stopped[i] == 0 || stoppedSp[i] == 0)
      continue;
    world->stacks[world->stackCount] = stoppedSp[i];
    ++world->stackCount;
    if (stoppedInterruptedSp[i] != 0) {
      world->stacks[world->stackCount] = stoppedInterruptedSp[i];
      ++world->stackCount;
    }
  }
  return 1;
}

static void resumeThreads(void) {
  __atomic_store_n(&stopping, 0, __ATOMIC_RELEASE);
  __atomic_add_fetch(&stopGeneration, 1, __ATOMIC_RELEASE);
  (void)futex(&stopGeneration, FUTEX_WAKE_PRIVATE, 0x7fffffff, NULL);
  /* Every stopped thread has left the handler's wait before the next stop
   * can reuse the table: they leave on the generation, which only grows. */
}

static int threadsChanged(void) {
  pid_t now[MaxThreads];
  int overflow;
  const unsigned count = listTasks(now, MaxThreads, &overflow);
  unsigned i;
  unsigned j;
  /* By identity: a thread that exited since is no root. */
  if (overflow)
    return 1;
  for (i = 0; i < count; ++i) {
    for (j = 0; j < taskCount && tasks[j] != now[i]; ++j) {
    }
    if (j == taskCount)
      return 1;
  }
  return 0;
}

/* Parses a hexadecimal number at *at. */
static uintptr_t hex(const char **at) {
  uintptr_t value = 0;
  for (;;) {
    const char c = **at;
    unsigned digit;
    if (c >= '0' && c <= '9')
      digit = (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f')
      digit = (unsigned)(c - 'a' + 10);
    else
      return value;
    value = value * 16 + digit;
    ++*at;
  }
}

/* /proc/self/maps, read whole into a buffer of the runtime's own. */
static char *mapsText;
static size_t mapsRoom;

static size_t readMaps(void) {
  size_t used = 0;
  const int fd = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
  if (fd < 0)
    return 0;
  for (;;) {
    long got;
    if (used + 4096 > mapsRoom) {
      const size_t room = mapsRoom == 0 ? (size_t)1 << 20 : mapsRoom * 2;
      char *grown = (char *)mmap(NULL, room, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if ((void *)grown == MAP_FAILED) {
        (void)close(fd);
        return 0;
      }
      weavecRtInternal((uintptr_t)grown, (uintptr_t)grown + room);
      if (mapsText != NULL) {
        memcpy(grown, mapsText, used);
        weavecRtInternalDrop((uintptr_t)mapsText);
        (void)munmap(mapsText, mapsRoom);
      }
      mapsText = grown;
      mapsRoom = room;
    }
    got = read(fd, mapsText + used, mapsRoom - used - 1);
    if (got < 0 && errno == EINTR)
      continue;
    if (got < 0) {
      (void)close(fd);
      return 0;
    }
    if (got == 0)
      break;
    used += (size_t)got;
  }
  (void)close(fd);
  mapsText[used] = 0;
  return used;
}

/* The resident pages of [low, high) (mincore), for the part of a mapping
 * below a stack pointer that is not a thread's own stack. With `pagemap`
 * (/proc/self/pagemap), for a private mapping the program cannot write,
 * the pages present or swapped that are not a file's: what it wrote
 * before it took the permission away (RFC 0037 section 1), and not the
 * pages of a mapped file, which every sweep would read again. */
static void scanResident(uintptr_t low, uintptr_t high, int pagemap) {
  enum { Pages = 512 };
  const uintptr_t page = (uintptr_t)getpagesize();
  unsigned char vector[Pages];
  uint64_t entries[Pages];
  low &= ~(page - 1);
  /* (Not the runtime's own: its reservations are large.) */
  while ((low = skipInternal(low)) < high) {
    const uintptr_t chunk =
        high - low > Pages * page ? Pages * page : high - low;
    const uintptr_t count = (chunk + page - 1) / page;
    const int mapped =
        pagemap >= 0 && pread(pagemap, entries, count * sizeof *entries,
                              (off_t)(low / page * sizeof *entries)) ==
                            (ssize_t)(count * sizeof *entries);
    uintptr_t i;
    if (!mapped && mincore((void *)low, (size_t)chunk, vector) != 0) {
      scanExcluding(low, low + chunk);
    } else {
      for (i = 0; i < count; ++i)
        /* Bits 63, 62 and 61: present, swapped, a file's page. */
        if (mapped ? (entries[i] >> 61) == 4 || ((entries[i] >> 62) & 1) != 0
                   : (vector[i] & 1) != 0) {
          const uintptr_t end = low + (i + 1) * page;
          scanExcluding(low + i * page, end < high ? end : high);
        }
    }
    low += chunk;
  }
}

/* Every readable mapping but code the program cannot write, a shared
 * mapping it cannot write through, a device's and the kernel's (RFC 0037
 * section 1); whether /proc/self/maps could be read. The main thread's
 * stack ("[stack]") is dead below its stack pointer; a thread's stack that
 * glibc mapped is not named, so below a stack pointer anywhere else the
 * resident pages are read: they may be other stacks of a coroutine pool. */
static int scanMappings(void) {
  const char *at;
  int pagemap;
  if (readMaps() == 0)
    return 0;
  pagemap = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
  at = mapsText;
  while (*at != 0) {
    const uintptr_t low = hex(&at);
    uintptr_t high;
    int readable;
    int writable;
    int executable;
    int shared;
    ++at;
    high = hex(&at);
    ++at;
    readable = at[0] == 'r';
    writable = at[1] == 'w';
    executable = at[2] == 'x';
    shared = at[3] == 's';
    if (readable && (writable || (!executable && !shared))) {
      /* [vvar] and [vsyscall] hold the kernel's data; a device mapping
       * might be readable. Shared memory under /dev is memory: POSIX
       * shared memory (/dev/shm), shared anonymous mappings ("/dev/zero
       * (deleted)") and hugetlbfs. */
      const char *name = at;
      while (*name != 0 && *name != '\n' && *name != '[' && *name != '/')
        ++name;
      if (strncmp(name, "[vvar", 5) == 0 || strncmp(name, "[vsys", 5) == 0) {
        /* The kernel's. */
      } else if (strncmp(name, "/dev/", 5) != 0 ||
                 strncmp(name, "/dev/shm/", 9) == 0 ||
                 strncmp(name, "/dev/zero", 9) == 0 ||
                 strncmp(name, "/dev/hugepages/", 15) == 0) {
        const uintptr_t sp = stackIn(low, high);
        if (!writable) {
          scanResident(low, high, pagemap);
        } else if (sp == 0 || strncmp(name, "[stack]", 7) == 0) {
          scanStack(low, high);
        } else {
          const uintptr_t live = sp - RedZone > low ? sp - RedZone : low;
          scanResident(low, live, -1);
          scanExcluding(live, high);
        }
      }
    }
    while (*at != 0 && *at != '\n')
      ++at;
    if (*at == '\n')
      ++at;
  }
  if (pagemap >= 0)
    (void)close(pagemap);
  return 1;
}

static void lockImages(void) {}
static void unlockImages(void) {}

#endif

/*===-- The sweep -----------------------------------------------------------===*/

static struct weavecRtSweepCounts lastCounts;
unsigned long long weavecRtSweepNanos;
unsigned long long weavecRtSweepLongest;

static int mapWorld(void) {
  void *map;
  if (world != NULL)
    return 1;
  map = mmap(NULL, sizeof(World), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map == MAP_FAILED)
    return 0;
  weavecRtInternal((uintptr_t)map, (uintptr_t)map + sizeof(World));
  world = (World *)map;
  map = mmap(NULL, WorkSlots * sizeof(Work), PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (map != MAP_FAILED) {
    weavecRtInternal((uintptr_t)map, (uintptr_t)map + WorkSlots * sizeof(Work));
    work = (Work *)map;
  }
  return 1;
}

void weavecRtSweep(void) {
  uint64_t started;
  struct weavecRtSweepCounts counts = {0, 0, 0};
  int stoppedAll;
  sigset_t every;
  sigset_t saved;
  /* The calling thread's callee-saved registers are spilled to this frame,
   * which its stack scan covers: it is scanned from `here`. */
  volatile uintptr_t here = 0;
  __builtin_unwind_init();
  if (weavecRtAllocatorNested())
    return;
  /* The sweeping thread's released slots join the quarantine, and its free
   * ones the free lists, so that what it released is recycled now and
   * slots are reused in address order after. */
  weavecRtFlushThread();
  if (!weavecRtSweepWanted() || !trySweepLock())
    return;
  if (!mapWorld()) {
    weavecRtSweepUnlock();
    return;
  }
  /* While it holds the allocator's locks the sweeping thread is the only
   * one running, so the kernel delivers the process's signals to it: a
   * handler that allocated would wait for itself (RFC 0037 section 1). */
  sigfillset(&every);
  (void)pthread_sigmask(SIG_BLOCK, &every, &saved);
  if (work != NULL)
    startWorkers();
  started = nanoseconds();
  lockImages();
  weavecRtLockAll();
  world->stackCount = 0;
  world->registerCount = 0;
  world->tsdCount = 0;
  stoppedAll = stopThreads();
  if (stoppedAll && weavecRtPrepareMarks()) {
    unsigned i;
    world->stacks[world->stackCount] = (uintptr_t)&here;
    ++world->stackCount;
    sortInternal();
    for (i = 0; i < world->registerCount; ++i)
      weavecRtMarkWords((uintptr_t)world->registers[i],
                        (uintptr_t)(world->registers[i] + Registers));
    /* Without every root, nothing is recycled. A large heap is marked by
     * the workers too (RFC 0037 section 3). */
    const int parallel = workerCount != 0 && weavecRtLiveBytes() >= ParallelBytes;
    int scanned;
    if (parallel) {
      workCount = 0;
      collecting = 1;
      scanned = scanMappings();
      collecting = 0;
      weavecRtHeapWork(addSlots, MarkChunk);
      runWork();
      weavecRtMarkHuge();
    } else {
      scanned = scanMappings();
      weavecRtMarkHeap();
    }
    weavecRtRecycle(&counts, scanned && !threadsChanged());
    weavecRtCount(WeavecRtStatSweeps);
    lastCounts = counts;
  }
  resumeThreads();
  weavecRtUnlockAll();
  unlockImages();
  (void)pthread_sigmask(SIG_SETMASK, &saved, NULL);
  {
    const uint64_t ended = nanoseconds();
    const uint64_t took = ended - started;
    weavecRtSweepNanos += took;
    if (took > weavecRtSweepLongest)
      weavecRtSweepLongest = took;
    weavecRtSwept(ended, took, stoppedAll);
  }
  weavecRtSweepUnlock();
}

void weavecRtLastSweep(struct weavecRtSweepCounts *out) { *out = lastCounts; }
