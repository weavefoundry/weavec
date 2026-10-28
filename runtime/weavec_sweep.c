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
|*      registers and stack, every readable and writable mapping of the
|*      process but the runtime's own (of the dyld shared cache, only the
|*      writable segments of the images the process loaded), and every live
|*      heap block;
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
 * mapping that holds it. */
enum { MaxThreads = 4096 };
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
  uintptr_t registers[MaxThreads][34];
  unsigned registerCount;
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

/* Scans [low, high) but the runtime's own mappings in it (sorted). */
static void scanExcluding(uintptr_t low, uintptr_t high) {
  unsigned i;
  for (i = 0; i < internalCount && low < high; ++i) {
    const uintptr_t a = internalRanges[i].low;
    const uintptr_t b = internalRanges[i].high;
    if (b <= low || a >= high)
      continue;
    if (a > low)
      weavecRtMarkWords(low, a);
    low = b;
  }
  if (low < high)
    weavecRtMarkWords(low, high);
}

/* Scans a writable mapping, a stack only from its stack pointer. */
static void scanMapping(uintptr_t low, uintptr_t high) {
  /* The lowest stack pointer in the mapping: threads whose stacks share one
   * mapping are all scanned from it. */
  uintptr_t sp = 0;
  unsigned i;
  for (i = 0; i < world->stackCount; ++i)
    if (world->stacks[i] >= low && world->stacks[i] < high &&
        (sp == 0 || world->stacks[i] < sp))
      sp = world->stacks[i];
  if (sp == 0)
    scanExcluding(low, high);
  else
    scanExcluding(sp - RedZone > low ? sp - RedZone : low, high);
}


#if defined(__APPLE__)

/*===-- Darwin --------------------------------------------------------------===*/

/* The first address at or after `address` outside the runtime's own
 * mappings (sorted). */
static uintptr_t skipInternal(uintptr_t address) {
  unsigned i;
  for (i = 0; i < internalCount; ++i)
    if (address >= internalRanges[i].low && address < internalRanges[i].high)
      address = internalRanges[i].high;
  return address;
}

static thread_act_array_t threads;
static mach_msg_type_number_t threadCount;

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
    if (threads[i] == self)
      continue;
    if (thread_suspend(threads[i]) != KERN_SUCCESS)
      ok = 0;
  }
  /* The threads not suspended are not resumed either. */
  for (; i < threadCount; ++i)
    if (threads[i] != self) {
      (void)mach_port_deallocate(mach_task_self(), threads[i]);
      threads[i] = MACH_PORT_NULL;
    }
  if (!ok)
    return 0;
  /* Each stopped thread's registers and stack pointer. */
  for (i = 0; i < threadCount; ++i) {
    if (threads[i] == self || threads[i] == MACH_PORT_NULL)
      continue;
#if defined(__aarch64__)
    {
      arm_thread_state64_t state;
      mach_msg_type_number_t count = ARM_THREAD_STATE64_COUNT;
      unsigned r;
      if (thread_get_state(threads[i], ARM_THREAD_STATE64,
                           (thread_state_t)&state, &count) != KERN_SUCCESS)
        return 0;
      for (r = 0; r < 29; ++r)
        world->registers[world->registerCount][r] = state.__x[r];
      world->registers[world->registerCount][29] =
          (uintptr_t)arm_thread_state64_get_fp(state);
      world->registers[world->registerCount][30] =
          (uintptr_t)arm_thread_state64_get_lr(state);
      world->stacks[world->stackCount] =
          (uintptr_t)arm_thread_state64_get_sp(state);
    }
#elif defined(__x86_64__)
    {
      x86_thread_state64_t state;
      mach_msg_type_number_t count = x86_THREAD_STATE64_COUNT;
      const uint64_t *raw = (const uint64_t *)&state;
      unsigned r;
      if (thread_get_state(threads[i], x86_THREAD_STATE64,
                           (thread_state_t)&state, &count) != KERN_SUCCESS)
        return 0;
      for (r = 0; r < sizeof state / sizeof(uint64_t) && r < 34; ++r)
        world->registers[world->registerCount][r] = raw[r];
      world->stacks[world->stackCount] = state.__rsp;
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
    if (threads[i] != self)
      (void)thread_resume(threads[i]);
    (void)mach_port_deallocate(mach_task_self(), threads[i]);
  }
  (void)vm_deallocate(mach_task_self(), (vm_address_t)threads,
                      threadCount * sizeof(thread_t));
  threads = NULL;
  threadCount = 0;
}

/* Whether a thread exists that was not stopped. */
static int threadsChanged(void) {
  thread_act_array_t now;
  mach_msg_type_number_t count;
  mach_msg_type_number_t i;
  int changed;
  if (task_threads(mach_task_self(), &now, &count) != KERN_SUCCESS)
    return 1;
  changed = count != threadCount;
  for (i = 0; i < count; ++i)
    (void)mach_port_deallocate(mach_task_self(), now[i]);
  (void)vm_deallocate(mach_task_self(), (vm_address_t)now,
                      count * sizeof(thread_t));
  return changed;
}

/* The pages of [low, high) the program may have written (mincore); a page
 * never touched, or a file's page never modified, holds nothing it wrote. */
static void scanResident(uintptr_t low, uintptr_t high) {
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
      scanMapping(low, low + chunk);
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
               (MINCORE_INCORE | MINCORE_ANONYMOUS));
      if (touched)
        continue;
      if (i > run) {
        const uintptr_t end = low + i * page < low + chunk ? low + i * page
                                                           : low + chunk;
        scanMapping(low + run * page, end);
      }
      run = i + 1;
    }
    low += chunk;
  }
}

#ifndef VM_MEMORY_VM_RECLAIM
#define VM_MEMORY_VM_RECLAIM 22
#endif

/* Whether a region may hold what the program wrote: readable and writable,
 * with pages written or paged out (an untouched page reads zero, a clean
 * page of a file holds the file), and not the kernel's reclaim ring, which
 * holds only the system allocator's free ranges. The page counts cost one
 * call per region; mincore then finds the pages, at about a microsecond
 * each. */
static int worthScanning(const vm_region_submap_info_data_64_t *info) {
  return (info->protection & (VM_PROT_READ | VM_PROT_WRITE)) ==
             (VM_PROT_READ | VM_PROT_WRITE) &&
         info->share_mode != SM_EMPTY &&
         (info->pages_dirtied != 0 || info->pages_swapped_out != 0) &&
         info->user_tag != VM_MEMORY_VM_RECLAIM;
}

/* The stack pointer of a stopped thread inside [low, high), or 0. */
static uintptr_t stackIn(uintptr_t low, uintptr_t high) {
  unsigned i;
  for (i = 0; i < world->stackCount; ++i)
    if (world->stacks[i] >= low && world->stacks[i] < high)
      return world->stacks[i];
  return 0;
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
 * cache's, whose loaded images' writable segments are scanned instead. */
static void scanMappings(void) {
  mach_vm_address_t address = 0;
  unsigned i;
  for (;;) {
    mach_vm_size_t size = 0;
    natural_t depth = 0;
    vm_region_submap_info_data_64_t info;
    mach_msg_type_number_t count = VM_REGION_SUBMAP_INFO_COUNT_64;
    /* The runtime's own reservations (the arena's committed chunks are a
     * region each) are skipped whole, not asked about one by one. */
    address = skipInternal(address);
    if (address >= cacheLow && address < cacheHigh)
      address = cacheHigh;
    if (mach_vm_region_recurse(mach_task_self(), &address, &size, &depth,
                               (vm_region_recurse_info_t)&info,
                               &count) != KERN_SUCCESS)
      break;
    if (!info.is_submap && worthScanning(&info) &&
        (address + size <= cacheLow || address >= cacheHigh)) {
      const uintptr_t low = (uintptr_t)address;
      const uintptr_t high = (uintptr_t)(address + size);
      /* A stack is resident from its stack pointer up. */
      if (stackIn(low, high) != 0)
        scanMapping(low, high);
      else
        scanResident(low, high);
    }
    address += size;
  }
  for (i = 0; i < imageSegmentCount; ++i)
    scanResident(imageSegments[i].low, imageSegments[i].high);
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
  for (i = 0; i < stopTargets; ++i)
    if (stopped[i] == self) {
      stoppedSp[i] = (uintptr_t)&here;
      {
        const ucontext_t *uc = (const ucontext_t *)context;
#if defined(__x86_64__)
        stoppedInterruptedSp[i] = (uintptr_t)uc->uc_mcontext.gregs[REG_RSP];
#elif defined(__aarch64__)
        stoppedInterruptedSp[i] = (uintptr_t)uc->uc_mcontext.sp;
#else
        (void)uc;
#endif
      }
      break;
    }
  __atomic_add_fetch(&stoppedCount, 1, __ATOMIC_RELEASE);
  (void)futex(&stoppedCount, FUTEX_WAKE_PRIVATE, 1, NULL);
  while (__atomic_load_n(&stopGeneration, __ATOMIC_ACQUIRE) == generation)
    (void)futex(&stopGeneration, FUTEX_WAIT_PRIVATE, generation, NULL);
  errno = saved;
}

/* The signal that stops threads: SIGPWR, WEAVEC_RT_SWEEP_SIGNAL, or SIGXCPU
 * when the program handles SIGPWR; 0 when none is free. */
__attribute__((constructor)) static void readSweepSignal(void) {
  const char *text = getenv("WEAVEC_RT_SWEEP_SIGNAL");
  stopSignal = text != NULL ? atoi(text) : 0;
}

static int installHandler(void) {
  static int installed;
  static const int Candidates[2] = {SIGPWR, SIGXCPU};
  int i;
  if (installed)
    return stopSignal != 0;
  installed = 1;
  for (i = stopSignal != 0 ? -1 : 0; i < 2; ++i) {
    const int signal = i < 0 ? stopSignal : Candidates[i];
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
      stopSignal = signal;
      return 1;
    }
  }
  stopSignal = 0;
  return 0;
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
  if (taskCount == 1)
    return 1;
  if (!installHandler())
    return 0;
  stopTargets = 0;
  for (i = 0; i < taskCount; ++i)
    if (tasks[i] != self) {
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
   * holds up recycling. */
  deadline = nanoseconds() + 1000000000ULL;
  for (;;) {
    const int now = __atomic_load_n(&stoppedCount, __ATOMIC_ACQUIRE);
    struct timespec wait = {0, 1000000};
    if ((unsigned)now >= sent)
      break;
    if (nanoseconds() > deadline)
      return 0;
    (void)futex(&stoppedCount, FUTEX_WAIT_PRIVATE, now, &wait);
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
  if (overflow || count != taskCount)
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
    if (got <= 0)
      break;
    used += (size_t)got;
  }
  (void)close(fd);
  mapsText[used] = 0;
  return used;
}

static void scanMappings(void) {
  const char *at;
  if (readMaps() == 0)
    return;
  at = mapsText;
  while (*at != 0) {
    const uintptr_t low = hex(&at);
    uintptr_t high;
    int readable;
    int writable;
    ++at;
    high = hex(&at);
    ++at;
    readable = at[0] == 'r';
    writable = at[1] == 'w';
    if (readable && writable) {
      /* [vvar] and [vsyscall] are never rw; a device mapping might be. */
      const char *name = at;
      while (*name != 0 && *name != '\n' && *name != '[' && *name != '/')
        ++name;
      if (strncmp(name, "/dev/", 5) != 0)
        scanMapping(low, high);
    }
    while (*at != 0 && *at != '\n')
      ++at;
    if (*at == '\n')
      ++at;
  }
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
  return 1;
}

void weavecRtSweep(void) {
  uint64_t started;
  struct weavecRtSweepCounts counts = {0, 0, 0};
  int stoppedAll;
  /* The calling thread's callee-saved registers are spilled to this frame,
   * which its stack scan covers: it is scanned from `here`. */
  volatile uintptr_t here = 0;
  __builtin_unwind_init();
  if (weavecRtForward() != 0 || !weavecRtSweepWanted() || !trySweepLock())
    return;
  if (!mapWorld()) {
    weavecRtSweepUnlock();
    return;
  }
  started = nanoseconds();
  lockImages();
  weavecRtLockAll();
  world->stackCount = 0;
  world->registerCount = 0;
  stoppedAll = stopThreads();
  if (stoppedAll && weavecRtPrepareMarks()) {
    unsigned i;
    world->stacks[world->stackCount] = (uintptr_t)&here;
    ++world->stackCount;
    sortInternal();
    for (i = 0; i < world->registerCount; ++i)
      weavecRtMarkWords((uintptr_t)world->registers[i],
                        (uintptr_t)(world->registers[i] + 34));
    scanMappings();
    weavecRtMarkHeap();
    weavecRtRecycle(&counts, !threadsChanged());
    weavecRtCount(WeavecRtStatSweeps);
    lastCounts = counts;
  }
  resumeThreads();
  weavecRtUnlockAll();
  unlockImages();
  {
    const uint64_t ended = nanoseconds();
    const uint64_t took = ended - started;
    weavecRtSweepNanos += took;
    if (took > weavecRtSweepLongest)
      weavecRtSweepLongest = took;
    weavecRtSwept(ended, took);
  }
  weavecRtSweepUnlock();
}

void weavecRtLastSweep(struct weavecRtSweepCounts *out) { *out = lastCounts; }
