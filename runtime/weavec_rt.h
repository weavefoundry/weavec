/*===- weavec_rt.h - The WeaveC runtime's internal interface ------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0035, section 5, and RFC 0036, section 5. Shared by the runtime's own
|* sources and its tests;
|* not installed. Compiled code reaches the runtime through the entry points
|* the guard pass calls (lib/Frontend/GuardPass*.cpp declares the same
|* ones), the shadow descriptor it reads inline, and the standard allocation
|* functions weavec_malloc.c defines.
|*
\*===----------------------------------------------------------------------===*/

#ifndef WEAVEC_RUNTIME_WEAVEC_RT_H
#define WEAVEC_RUNTIME_WEAVEC_RT_H

#include <stdarg.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <wchar.h>

/* The entry points the library exports (runtime/weavec_rt.exports lists
 * them); everything else the runtime defines is hidden. */
#define WEAVEC_RT_API __attribute__((visibility("default")))
/* An entry point a program may define itself (an allocation function, ASan's
 * and UBSan's interface): weak on ELF, so that a -static program's own links
 * beside the archive. Not on Darwin, where ld64 would take an archive
 * member's strong definition over the library's weak one. */
#if defined(__APPLE__)
#define WEAVEC_RT_REPLACEABLE WEAVEC_RT_API
#else
#define WEAVEC_RT_REPLACEABLE WEAVEC_RT_API __attribute__((weak))
#endif

/* The guards' slow paths keep every register of their caller, so that an
 * inline guard costs the code around it no spills and the register
 * allocator no live-range splits; the guard pass calls them so on AArch64
 * and x86-64. Clang builds the runtime. */
#if defined(__aarch64__) || defined(__x86_64__)
#define WEAVEC_RT_SLOW __attribute__((preserve_all))
#else
#define WEAVEC_RT_SLOW
#endif

/*===-- The shadow (section 2.2) ------------------------------------------===*/

/* One byte per 16-byte granule of a window of the address space, at
 * base + ((address >> 4) & mask). The guard pass reads `base` and `mask` at
 * offsets 0 and 8 once per function. Until the shadow is reserved, and when
 * it cannot be, `mask` is 0 and `base` points to zero bytes the frame code
 * may scribble on: every guard then passes, because the slow path knows. */
struct __weavec_rt_shadow_t {
  uintptr_t base;
  uintptr_t mask;
  /* Addresses at or above 1 << bits alias lower ones (section 2.2). */
  unsigned bits;
};

WEAVEC_RT_API extern struct __weavec_rt_shadow_t __weavec_rt_shadow;

/* Shadow values. 0: the granule's 16 bytes are addressable; 1-15: its first
 * k bytes are; the rest name why bytes are not. */
/* RFC 0037 section 7.1: 0x00-0x7F, the whole granule is addressable, in an
 * object of this color (0: untracked or uncolored); 0x80-0xEF, `1ccc
 * kkkk`, its first k bytes (1-15) are, in an object whose color's low three
 * bits are ccc (a color's are never 111); 0xF0-0xFF, poison. */
enum {
  WeavecRtShadowAddressable = 0x00,
  WeavecRtShadowGranule = 16,
  WeavecRtShadowPoisonFrom = 0xF0,
  WeavecRtShadowStackLeft = 0xF1,
  WeavecRtShadowStackMid = 0xF2,
  WeavecRtShadowStackRight = 0xF3,
  WeavecRtShadowStackDynamic = 0xF4,
  /* RFC 0037 section 9: a frame that returned. */
  WeavecRtShadowReturned = 0xF5,
  /* RFC 0036 section 5: poisoned by the program through ASan's interface. */
  WeavecRtShadowUser = 0xF7,
  WeavecRtShadowStackScope = 0xF8,
  WeavecRtShadowGlobal = 0xF9,
  WeavecRtShadowHeapTail = 0xFA,
  /* RFC 0039 section 5: a container's storage past its elements. */
  WeavecRtShadowContainer = 0xFC,
  WeavecRtShadowHeapFreed = 0xFD,
  /* The lowest WeavecRtNullPage bytes: an access there is through null. */
  WeavecRtShadowNull = 0xFE
};
enum { WeavecRtNullPage = 65536 };

static inline int weavecRtShadowFull(unsigned char v) { return v < 0x80; }
static inline int weavecRtShadowPoison(unsigned char v) {
  return v >= WeavecRtShadowPoisonFrom;
}
/* The addressable bytes of a granule that is neither whole nor poison. */
static inline unsigned weavecRtShadowBytes(unsigned char v) { return v & 15U; }
static inline unsigned char weavecRtShadowPartialOf(unsigned color,
                                                    unsigned k) {
  return (unsigned char)(0x80 | ((color & 7U) << 4) | k);
}
/* The low three bits of a granule's color, whole or partial. */
static inline unsigned weavecRtShadowClass(unsigned char v) {
  return v < 0x80 ? (v & 7U) : ((v >> 4) & 7U);
}
/* The n-th color: 1 <= color <= 126, low bits never 7; n apart by less
 * than 111 differ. */
static inline unsigned char weavecRtColor(uint64_t n) {
  const unsigned m = (unsigned)(n % 111) + 1;
  return (unsigned char)(((m / 7) << 3) | (m % 7));
}

static inline int weavecRtShadowReady(void) {
  return __atomic_load_n(&__weavec_rt_shadow.mask, __ATOMIC_ACQUIRE) != 0;
}

/* Whether the shadow describes `address` itself rather than an alias. */
static inline int weavecRtInWindow(uintptr_t address) {
  return __weavec_rt_shadow.bits >= 64 ||
         (address >> __weavec_rt_shadow.bits) == 0;
}

static inline unsigned char *weavecRtShadowOf(uintptr_t address) {
  return (unsigned char *)(__weavec_rt_shadow.base +
                           ((address >> 4) & __weavec_rt_shadow.mask));
}

/* Whether the n bytes at `address` are addressable, decided from one
 * eight-byte load of the shadow (the inline guard's chunk check); 0 when
 * that cannot tell (more than 113 bytes, or a poisoned byte), for the full
 * check to decide. */
static inline int weavecRtQuickOk(uintptr_t address, uint64_t n) {
  uint64_t last;
  unsigned shift;
  uint64_t chunk;
  unsigned char value;
  if (n == 0)
    return 1;
  if (n > 113 || !weavecRtShadowReady() || !weavecRtInWindow(address))
    return 0;
  last = (address & 15) + n - 1;
  shift = (unsigned)(last >> 4) * 8;
  /* (The shadow has room for an eight-byte load past its last byte.) */
  __builtin_memcpy(&chunk, weavecRtShadowOf(address), sizeof chunk);
  if (shift != 0 &&
      (chunk & (((uint64_t)1 << shift) - 1) & 0x8080808080808080ULL) != 0)
    return 0;
  value = (unsigned char)(chunk >> shift);
  return weavecRtShadowFull(value) ||
         (!weavecRtShadowPoison(value) &&
          (last & 15) < weavecRtShadowBytes(value));
}

/* Writes `value` over the shadow of [base, base + size), 16-aligned base,
 * whole granules (the last one partial counts whole). */
void weavecRtShadowSet(uintptr_t base, size_t size, unsigned char value);

/* Marks [base, base + size) addressable, in an object of `color`, and the
 * rest of its last granule not, with `tail`; base is 16-aligned. */
void weavecRtShadowObject(uintptr_t base, size_t size, unsigned char tail,
                          unsigned char color);
/* RFC 0037 section 7.2: the color of the heap slot that holds `address`, or
 * 0 outside the arena. */
unsigned char weavecRtColorOfHeap(uintptr_t address);

/* Clears the shadow of [low, high), rounded outwards to granules; whole
 * pages of it by mapping them afresh. */
void weavecRtShadowClear(uintptr_t low, uintptr_t high);

/* The calling thread's stack top (its highest address) and bottom, or 0. */
uintptr_t weavecRtStackTop(void);
uintptr_t weavecRtStackBottom(void);

struct __weavec_rt_site;

/*===-- The arena (sections 5.1) -------------------------------------------===*/

enum {
  /* 16, 32, ..., 128, then four classes per doubling up to 2^30. */
  WeavecRtClassCount = 100,
  /* Regions: one per class, and spares a class takes when its own is full
   * (RFC 0037 section 2.3). */
  WeavecRtRegionCount = 160,
  WeavecRtSmallClasses = 8,
  /* (RFC 0039 section 4: a live slot word holds a size below 2^28.) */
  WeavecRtMaxClassShift = 28,
  /* A dead slot at least this large gives its pages back. */
  WeavecRtDecommitBytes = 64 * 1024
};

/* One size class: slot = ((offset >> shift) * magic) >> 32. */
struct __weavec_rt_class {
  uint32_t size;
  uint32_t magic;
  uint32_t shift;
  /* Slots in a region. */
  uint32_t capacity;
};

/* What a lookup of an arena pointer needs; `bytes` is 0 until the arena
 * exists, so every pointer is outside it. */
struct __weavec_rt_heap_t {
  uintptr_t base;
  /* WeavecRtRegionCount << shift. */
  uintptr_t bytes;
  /* Region r's slot words start at meta + (r << (shift - 2)). */
  uintptr_t meta;
  uint32_t shift;
  /* Classes, and regions in use (the classes' own and the spares taken):
   * region r < classes serves class r. */
  uint32_t classes;
  uint32_t regions;
  /* Region r's class geometry (a spare's is its class's). */
  struct __weavec_rt_class table[WeavecRtRegionCount];
};

WEAVEC_RT_API extern struct __weavec_rt_heap_t __weavec_rt_heap;

/* A free or dead slot's word is `next << 2 | state`, the index + 1 of the
 * next slot on its list (0 for none); a live slot's is `size << 4 | form <<
 * 2 | state` (RFC 0039 section 4: what allocated it). */
enum {
  WeavecRtNever = 0,
  WeavecRtLive = 1,
  WeavecRtFree = 2,
  WeavecRtDead = 3,
  WeavecRtWordState = 3,
  WeavecRtWordShift = 2,
  WeavecRtLiveShift = 4
};
/* (WeavecRtFormAny: a release that takes a block of any form.) */
enum { WeavecRtFormMalloc, WeavecRtFormNew, WeavecRtFormNewArray, WeavecRtFormAny };

static inline uint32_t weavecRtWordValue(uint32_t word) {
  return word >> WeavecRtWordShift;
}
static inline uint32_t weavecRtLiveSize(uint32_t word) {
  return word >> WeavecRtLiveShift;
}
static inline unsigned weavecRtLiveForm(uint32_t word) {
  return (word >> WeavecRtWordShift) & 3U;
}

WEAVEC_RT_API void *__weavec_rt_alloc(size_t size, size_t alignment);
WEAVEC_RT_API void __weavec_rt_free(void *p);
WEAVEC_RT_API void *__weavec_rt_realloc(void *p, size_t size);
/* The requested size of a live block; of any other pointer, what the next
 * allocator says, or 0. */
WEAVEC_RT_API size_t __weavec_rt_size(const void *p);
/* RFC 0039 section 4: an allocation by `form`; a release by its release
 * (free, operator delete, operator delete[]: weavecRtReleaseName) of `size`
 * bytes, SIZE_MAX when not sized, called from `caller` (or null); the form a
 * release called from `caller` checks (any, from Darwin's shared cache:
 * amendment 1); a block another allocator's call returned recorded as
 * `form`; an invalid release by another form (`size` SIZE_MAX) or size. */
void *weavecRtAllocForm(size_t size, size_t alignment, unsigned form);
/* (RFC 0040 section 5.5: `site` is a guarded call's, or null.) */
void weavecRtReleaseForm(void *p, unsigned form, size_t size,
                         const void *caller,
                         const struct __weavec_rt_site *site);
void *weavecRtReallocAt(void *p, size_t size,
                        const struct __weavec_rt_site *site);
const char *weavecRtReleaseName(unsigned form);
unsigned weavecRtCallerForm(unsigned form, const void *caller);
void weavecRtSetForm(void *p, unsigned form);

/* RFC 0040 section 5.5: a bad release of `p` by `by` (free, realloc,
 * operator delete[...]): `why` it is no live block's start (null: inside
 * one); the allocation's form, or (`size`) a sized release's size. */
enum { WeavecRtDoubleFree, WeavecRtInvalidFree, WeavecRtMismatch };
struct weavecRtRelease {
  unsigned kind;
  const char *by;
  const void *p;
  const char *why;
  unsigned allocatedBy;
  size_t size;
  const struct __weavec_rt_site *site;
};
void weavecRtFailRelease(const struct weavecRtRelease *release)
    __attribute__((noreturn));

/* What the allocator knows of an address, for reports: whether it is in a
 * heap slot or huge block, that object's start, size, state and form, and
 * its allocation's and release's stacks (RFC 0040 section 3; 0: none). */
struct weavecRtHeapObject {
  int found;
  int live;
  uintptr_t base;
  size_t size;
  unsigned form;
  uint32_t allocated;
  uint32_t released;
};
struct weavecRtHeapObject weavecRtHeapObjectAt(uintptr_t address);

/*===-- Guards (section 5.2) -----------------------------------------------===*/

/* The constant record the guard pass emits per guarded site. */
enum {
  WeavecRtSiteWrite = 1,
  /* Report mode: report once and return instead of trapping. */
  WeavecRtSiteReport = 2,
  /* Verify mode's monitor of a guard section 6 removed. */
  WeavecRtSiteProven = 4,
  /* A string the call accepts as null (`system(NULL)`). */
  WeavecRtSiteNullOk = 8
};
/* RFC 0037 section 11: the file name lies `file` bytes from the record (0:
 * none), so that the records and the names are read-only data without a
 * relocation the loader applies; a copy is re-based (weavecRtSiteSet). */
struct __weavec_rt_site {
  int64_t file;
  unsigned line;
  unsigned short column;
  unsigned short flags;
};

static inline const char *
weavecRtSiteFile(const struct __weavec_rt_site *site) {
  return site != NULL && site->file != 0 ? (const char *)site + site->file
                                         : NULL;
}

static inline void weavecRtSiteSet(struct __weavec_rt_site *site,
                                   const char *file, unsigned line,
                                   unsigned column, unsigned flags) {
  site->file = file != NULL ? (int64_t)((intptr_t)file - (intptr_t)site) : 0;
  site->line = line;
  site->column = (unsigned short)(column > 0xFFFF ? 0xFFFF : column);
  site->flags = (unsigned short)flags;
}

/* The slow path of an inline guard of `width` bytes at `address`: returns
 * when the bytes are addressable; else traps, or reports and returns. */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_guard(uintptr_t address, uint64_t width,
                  const struct __weavec_rt_site *site);
/* RFC 0037 section 8.3: the access's granule and its root's differ in
 * color; `rootShadow` is the root's shadow byte as the guard read it. */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_jump(uintptr_t address, uint64_t width, uintptr_t root,
                 uint64_t rootShadow, const struct __weavec_rt_site *site);
/* An inline check with a root whose byte was poison, a partial granule's
 * end, or of another color: __weavec_rt_guard, then __weavec_rt_jump. */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_guard_jump(uintptr_t address, uint64_t width, uintptr_t root,
                       uint64_t rootShadow,
                       const struct __weavec_rt_site *site);
/* RFC 0037 section 8.5: whether a loop range from `root` starting at
 * `address` may run without its color checks. */
WEAVEC_RT_API int __weavec_rt_color_ok(uintptr_t root, uintptr_t address);
/* A guard of a base that may be null (section 2.4). */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_null(uintptr_t address, const struct __weavec_rt_site *site);
/* A range guard; `n` may be 0. */
WEAVEC_RT_API void __weavec_rt_range(uintptr_t address, uint64_t n,
                                     const struct __weavec_rt_site *site);
/* Whether the range is addressable, without a report (section 6.4). */
WEAVEC_RT_API int __weavec_rt_range_ok(uintptr_t address, uint64_t n);
/* The length of the string at `s`, checking that its bytes and terminator
 * are addressable; `max` bounds the scan (the bytes read are then at most
 * `max`). */
WEAVEC_RT_API uint64_t __weavec_rt_strlen(const char *s, uint64_t max,
                                          const struct __weavec_rt_site *site);

/* A copy whose operands overlap (section 2.5). */
WEAVEC_RT_API void __weavec_rt_overlap(uintptr_t destination, uintptr_t source,
                                       uint64_t n,
                                       const struct __weavec_rt_site *site);

/* Checked library calls (section 2.5). A writing call whose row has a
 * fortified alias takes `limit`, the size of the object it writes at most
 * (RFC 0036 section 6: a fortified call's object size; SIZE_MAX when
 * unknown): after the call's arguments and before the site, or right after
 * the site when the call is variadic. */
WEAVEC_RT_API char *__weavec_rt_strcpy(char *d, const char *s, size_t limit,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_stpcpy(char *d, const char *s, size_t limit,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_strcat(char *d, const char *s, size_t limit,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_sprintf(const struct __weavec_rt_site *site,
                                      size_t limit, char *d,
                                      const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vsprintf(char *d, const char *format,
                                       va_list arguments, size_t limit,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_snprintf(const struct __weavec_rt_site *site,
                                       size_t limit, char *d, size_t n,
                                       const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vsnprintf(char *d, size_t n, const char *format,
                                        va_list arguments, size_t limit,
                                        const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_gets(char *d, size_t limit,
                                     const struct __weavec_rt_site *site);
/* The scanf family checks, after the call, what each conversion that ran
 * wrote. */
WEAVEC_RT_API int __weavec_rt_sscanf(const struct __weavec_rt_site *site,
                                     const char *s, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vsscanf(const char *s, const char *format,
                                      va_list arguments,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_fscanf(const struct __weavec_rt_site *site,
                                     FILE *stream, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vfscanf(FILE *stream, const char *format,
                                      va_list arguments,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_scanf(const struct __weavec_rt_site *site,
                                    const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vscanf(const char *format, va_list arguments,
                                     const struct __weavec_rt_site *site);
/* The comparisons check the bytes they read, up to where they stop (the
 * searches are checked by the guards, after the call: RFC 0038 section 2). */
WEAVEC_RT_API int __weavec_rt_strcmp(const char *a, const char *b,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strncmp(const char *a, const char *b, size_t n,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strcasecmp(const char *a, const char *b,
                                         const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_strncasecmp(const char *a, const char *b,
                                          size_t n,
                                          const struct __weavec_rt_site *site);
/* RFC 0036 section 6: the number parsers check, after the call, the bytes
 * the C library read: up to the end pointer and the character that stopped
 * it, or, when nothing was converted, the leading white space and sign and
 * the character after them. */
WEAVEC_RT_API long __weavec_rt_strtol(const char *s, char **end, int base,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API long long __weavec_rt_strtoll(const char *s, char **end,
                                            int base,
                                            const struct __weavec_rt_site *site);
WEAVEC_RT_API unsigned long
__weavec_rt_strtoul(const char *s, char **end, int base,
                    const struct __weavec_rt_site *site);
WEAVEC_RT_API unsigned long long
__weavec_rt_strtoull(const char *s, char **end, int base,
                     const struct __weavec_rt_site *site);
WEAVEC_RT_API intmax_t __weavec_rt_strtoimax(const char *s, char **end,
                                             int base,
                                             const struct __weavec_rt_site *site);
WEAVEC_RT_API uintmax_t
__weavec_rt_strtoumax(const char *s, char **end, int base,
                      const struct __weavec_rt_site *site);
WEAVEC_RT_API double __weavec_rt_strtod(const char *s, char **end,
                                        const struct __weavec_rt_site *site);
WEAVEC_RT_API float __weavec_rt_strtof(const char *s, char **end,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API long double
__weavec_rt_strtold(const char *s, char **end,
                    const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_atoi(const char *s,
                                   const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_atol(const char *s,
                                    const struct __weavec_rt_site *site);
WEAVEC_RT_API long long __weavec_rt_atoll(const char *s,
                                          const struct __weavec_rt_site *site);
WEAVEC_RT_API double __weavec_rt_atof(const char *s,
                                      const struct __weavec_rt_site *site);
/* The mappings clear the shadow of what they map and unmap. */
WEAVEC_RT_API void *__weavec_rt_mmap(void *address, size_t length,
                                     int protection, int flags, int fd,
                                     long long offset,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_munmap(void *address, size_t length,
                                     const struct __weavec_rt_site *site);

/* RFC 0037 section 5: the calls whose bytes are a C type's size, a length
 * read through a pointer, or the buffers an array of iovec names; the narrow
 * printf family, whose format the wrapper reads; and Darwin's calls of the
 * kind. (Types the header does not include are declared incomplete;
 * socklen_t is `unsigned`, time_t `long`, off_t `int64_t`, locale_t a
 * pointer.) */
struct iovec;
struct msghdr;
struct sockaddr;
struct stat;
struct tm;
struct utsname;
WEAVEC_RT_API long __weavec_rt_readv(int fd, const struct iovec *v, int count,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_writev(int fd, const struct iovec *v, int count,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_preadv(int fd, const struct iovec *v, int count,
                                      int64_t offset,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_pwritev(int fd, const struct iovec *v,
                                       int count, int64_t offset,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_recvmsg(int fd, struct msghdr *m, int flags,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_sendmsg(int fd, const struct msghdr *m,
                                       int flags,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_getline(char **line, size_t *n, FILE *stream,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_getdelim(char **line, size_t *n, int delimiter,
                                        FILE *stream,
                                        const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_inet_pton(int family, const char *text,
                                        void *address,
                                        const struct __weavec_rt_site *site);
WEAVEC_RT_API const char *
__weavec_rt_inet_ntop(int family, const void *address, char *text,
                      unsigned size, const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_stat(const char *path, struct stat *out,
                                   const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_lstat(const char *path, struct stat *out,
                                    const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_fstat(int fd, struct stat *out,
                                    const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_fstatat(int fd, const char *path,
                                      struct stat *out, int flags,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API struct tm *__weavec_rt_gmtime_r(const long *time, struct tm *out,
                                              const struct __weavec_rt_site *site);
WEAVEC_RT_API struct tm *
__weavec_rt_localtime_r(const long *time, struct tm *out,
                        const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_uname(struct utsname *out,
                                    const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_getsockopt(int fd, int level, int name,
                                         void *value, unsigned *length,
                                         const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_getsockname(int fd, struct sockaddr *address,
                                          unsigned *length,
                                          const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_getpeername(int fd, struct sockaddr *address,
                                          unsigned *length,
                                          const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_accept(int fd, struct sockaddr *address,
                                     unsigned *length,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API long __weavec_rt_recvfrom(int fd, void *buffer, size_t n,
                                        int flags, struct sockaddr *address,
                                        unsigned *length, size_t limit,
                                        const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_if_indextoname(unsigned index, char *name,
                                               const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_printf(const struct __weavec_rt_site *site,
                                     const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vprintf(const char *format, va_list arguments,
                                      const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_fprintf(const struct __weavec_rt_site *site,
                                      FILE *stream, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vfprintf(FILE *stream, const char *format,
                                       va_list arguments,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_dprintf(const struct __weavec_rt_site *site,
                                      int fd, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vdprintf(int fd, const char *format,
                                       va_list arguments,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_asprintf(const struct __weavec_rt_site *site,
                                       char **out, const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vasprintf(char **out, const char *format,
                                        va_list arguments,
                                        const struct __weavec_rt_site *site);
#if defined(__APPLE__)
WEAVEC_RT_API int __weavec_rt_sysctl(int *name, unsigned count, void *old,
                                     size_t *length, void *new_,
                                     size_t newLength,
                                     const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_sysctlbyname(const char *name, void *old,
                                           size_t *length, void *new_,
                                           size_t newLength,
                                           const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_basename_r(const char *path, char *base,
                                           const struct __weavec_rt_site *site);
WEAVEC_RT_API char *__weavec_rt_dirname_r(const char *path, char *base,
                                          const struct __weavec_rt_site *site);
WEAVEC_RT_API int __weavec_rt_sprintf_l(const struct __weavec_rt_site *site,
                                        char *d, void *locale,
                                        const char *format, ...);
WEAVEC_RT_API int __weavec_rt_snprintf_l(const struct __weavec_rt_site *site,
                                         char *d, size_t n, void *locale,
                                         const char *format, ...);
WEAVEC_RT_API int __weavec_rt_vsnprintf_l(char *d, size_t n, void *locale,
                                          const char *format,
                                          va_list arguments,
                                          const struct __weavec_rt_site *site);
#endif
/* A wide string's checked length in elements, at most `max`. */
WEAVEC_RT_API uint64_t __weavec_rt_wcslen(const wchar_t *s, uint64_t max,
                                          const struct __weavec_rt_site *site);

/* Frames (RFC 0035 section 3). */
WEAVEC_RT_API void __weavec_rt_unpoison_stack(void);
/* RFC 0039 section 3: at a landing pad, marks the frames the exception
 * unwound returned; the interposers say where it was raised and caught. */
WEAVEC_RT_API void __weavec_rt_landed(void);
void weavecRtRaising(uintptr_t frame);
void weavecRtCaught(uintptr_t frame);
/* A dynamic stack object of `size` bytes at `address` (32-aligned), followed
 * by its redzone up to the next 32-byte boundary and 32 bytes more. */
WEAVEC_RT_API void __weavec_rt_alloca_poison(uintptr_t address, uint64_t size);
/* Clears the shadow of [low, high): the dynamic objects a stack restore or a
 * return frees. */
WEAVEC_RT_API void __weavec_rt_alloca_unpoison(uintptr_t low, uintptr_t high);
/* RFC 0037 section 4: `value` over the shadow of the frame bytes
 * [address, address + bytes) that lie on the calling thread's stack. */
WEAVEC_RT_API void __weavec_rt_frame_shadow(uintptr_t address, uint64_t bytes,
                                            uint64_t value);

/* A fortified call that writes `width` bytes at `address`, more than the
 * `limit` its object size allows (RFC 0036 section 6). */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_limit_fail(uintptr_t address, uint64_t width, uint64_t limit,
                       const struct __weavec_rt_site *site);

/* RFC 0041 section 4: an access of `width` bytes at `address` in a safe
 * function, outside its pointer's bounds [lo, hi). */
WEAVEC_RT_API WEAVEC_RT_SLOW void
__weavec_rt_bounds_fail(uintptr_t address, uint64_t width, uintptr_t lo,
                        uintptr_t hi, const struct __weavec_rt_site *site);
/* RFC 0041 section 5: a safe function's escaping parameter must not point
 * into the calling thread's stack (`stack-escape`). */
WEAVEC_RT_API void
__weavec_rt_escape_check(uintptr_t address,
                         const struct __weavec_rt_site *site);

/* RFC 0039 section 6: a failed check of the C++ library (libc++'s trap,
 * libstdc++'s assertion function, libc++'s verbose abort), reported as
 * `library-assertion`. */
WEAVEC_RT_API void
__weavec_rt_library_assertion(const char *message,
                              const struct __weavec_rt_site *site)
    __attribute__((noreturn));
WEAVEC_RT_API void __weavec_rt_glibcxx_assert_fail(
    const char *file, int line, const char *function, const char *condition,
    const struct __weavec_rt_site *site) __attribute__((noreturn));
WEAVEC_RT_API void
__weavec_rt_libcpp_verbose_abort(const struct __weavec_rt_site *site,
                                 const char *format, ...)
    __attribute__((noreturn));

/* Globals (section 4): `n` records {address, size, size with redzone, and
 * (RFC 0040 section 5.4) the global's name and "<file>:<line>", each as an
 * offset from the record (0: none)}. */
struct __weavec_rt_global {
  uintptr_t address;
  uint64_t size;
  uint64_t padded;
  int64_t name;
  int64_t location;
};
/* RFC 0040 section 5.3: the descriptor a frame names in its left redzone
 * ({WeavecRtFrameMagic, descriptor}): its function, size (`reserved`) and
 * locals, each name an offset from its record. */
struct __weavec_rt_frame_object {
  uint32_t offset;
  uint32_t size;
  int64_t name;
};
struct __weavec_rt_frame {
  int64_t function;
  uint32_t objects;
  uint32_t reserved;
  struct __weavec_rt_frame_object object[];
};
#define WeavecRtFrameMagic (0x5745415645430000ULL | WEAVEC_RT_ABI)
/* A unit's globals and its frames' descriptors (`m` pointers). */
WEAVEC_RT_API void
__weavec_rt_globals_register(const struct __weavec_rt_global *globals,
                             uint64_t n,
                             const struct __weavec_rt_frame *const *frames,
                             uint64_t m);
WEAVEC_RT_API void
__weavec_rt_globals_unregister(const struct __weavec_rt_global *globals,
                               uint64_t n,
                               const struct __weavec_rt_frame *const *frames,
                               uint64_t m);
/* The registered global whose padded extent holds `address`, or null;
 * whether `frame` is a registered descriptor. */
const struct __weavec_rt_global *weavecRtGlobalAt(uintptr_t address);
int weavecRtFrameKnown(const struct __weavec_rt_frame *frame);

/* RFC 0040 sections 5.5, 9: a guarded call's release, with its site. */
WEAVEC_RT_API void __weavec_rt_free_at(void *p,
                                       const struct __weavec_rt_site *site);
WEAVEC_RT_API void *__weavec_rt_realloc_at(void *p, size_t size,
                                           const struct __weavec_rt_site *site);
WEAVEC_RT_API void *
__weavec_rt_reallocf_at(void *p, size_t size,
                        const struct __weavec_rt_site *site);
WEAVEC_RT_API void *
__weavec_rt_reallocarray_at(void *p, size_t count, size_t size,
                            const struct __weavec_rt_site *site);
WEAVEC_RT_API void __weavec_rt_delete_at(void *p,
                                         const struct __weavec_rt_site *site);
WEAVEC_RT_API void
__weavec_rt_delete_sized_at(void *p, size_t size,
                            const struct __weavec_rt_site *site);
WEAVEC_RT_API void
__weavec_rt_delete_array_at(void *p, const struct __weavec_rt_site *site);
WEAVEC_RT_API void
__weavec_rt_delete_array_sized_at(void *p, size_t size,
                                  const struct __weavec_rt_site *site);

/*===-- Reports (section 5.3) ----------------------------------------------===*/

/* Reports a failed guard of `kind` (a report kind's name) and traps, unless
 * the site asks for a report, which is printed once per site. */
void weavecRtFail(const char *kind, uintptr_t address, uint64_t width,
                  const struct __weavec_rt_site *site);
/* RFC 0037 section 8.3: a report whose second line names the root the
 * access was computed from, in another object. */
void weavecRtFailJump(const char *kind, uintptr_t address, uint64_t width,
                      uintptr_t root, const struct __weavec_rt_site *site);
/* As weavecRtFail, with a second line that places `address` relative to
 * [lo, hi), which `what` names ("destination": RFC 0036 section 6; "bounds
 * of the pointer": RFC 0041 section 4). */
void weavecRtFailObject(const char *kind, const char *what, uintptr_t address,
                        uint64_t width, uintptr_t lo, uintptr_t hi,
                        const struct __weavec_rt_site *site);
/* A report whose first line ends in `message` (library-assertion). */
void weavecRtFailMessage(const char *kind, const char *message,
                         const struct __weavec_rt_site *site)
    __attribute__((noreturn));
/* Called by a failure right before it traps, so that the trap ends the
 * program even where the program blocked or caught SIGTRAP and SIGILL. */
WEAVEC_RT_API void __weavec_rt_trapping(void);
/* Prints `weavec: error: <message>` and traps: the runtime cannot go on
 * (an allocation `operator new` cannot fail, no unwinder). */
void weavecRtDie(const char *message) __attribute__((noreturn));

/*===-- Stacks (RFC 0040 sections 1 to 4) ----------------------------------===*/

/* Frames a heap block's stack keeps by default and at most, and a
 * report's own stack. */
enum { WeavecRtHeapFrames = 8, WeavecRtHeapFramesMax = 32 };
enum { WeavecRtReportFrames = 32 };
/* The calling thread's number (T<n>; the main thread is T0) and stack. */
unsigned weavecRtThread(void);
void weavecRtOwnStack(uintptr_t *low, uintptr_t *high);
/* Up to `max` call addresses from the frame records at `fp`, after `pc`
 * (a faulting instruction) or else past the runtime's own frames. */
unsigned weavecRtUnwind(uintptr_t *frames, unsigned max, uintptr_t fp,
                        uintptr_t pc);
/* The store: a stack's identifier (0: not recorded), the identifier of the
 * stack walked from `fp` within [low, high), a stored stack, statistics. */
uint32_t weavecRtStackStore(const uintptr_t *frames, unsigned count,
                            unsigned thread);
uint32_t weavecRtStackId(uintptr_t fp, uintptr_t low, uintptr_t high,
                         unsigned thread);
unsigned weavecRtStackGet(uint32_t id, uintptr_t *frames, unsigned max,
                          unsigned *thread);
void weavecRtStoreStats(size_t *entries, size_t *bytes);
/* The frames a heap block's stack keeps (WEAVEC_RT_HEAP_STACKS; 0: none). */
extern unsigned weavecRtHeapStacks;
/* A mapping of the runtime's that neither the sweep nor a core dump reads. */
void weavecRtExcluded(void *p, size_t bytes);

/* Symbolization (section 4), only while reporting: each pc's module,
 * offset and lines {function, file, line, column}, the innermost first. */
struct weavecRtSymbol {
  const char *function;
  const char *file;
  unsigned line;
  unsigned column;
};
struct weavecRtFrameInfo {
  const char *module;
  uintptr_t offset;
  unsigned count;
  struct weavecRtSymbol lines[4];
};
void weavecRtSymbolize(const uintptr_t *pcs, unsigned count,
                       struct weavecRtFrameInfo *out);

/*===-- One runtime per process (RFC 0038 section 1) -----------------------===*/

/* The library's interface version (libweavec_rt.so.<abi>,
 * libweavec_rt.<abi>.dylib), raised whenever an entry point, the shadow
 * encoding or a site record changes. */
#define WEAVEC_RT_ABI 41

/*===-- Internal -----------------------------------------------------------===*/

/* Reserves the shadow and this copy's arena on first use; 0 when there is
 * no room for the arena. */
int weavecRtInitialize(void);

/* The runtime's own mappings, which the sweep does not scan. */
void weavecRtInternal(uintptr_t low, uintptr_t high);
void weavecRtInternalDrop(uintptr_t low);

/* The sweep (weavec_sweep.c), and the allocator's side of it. */
struct weavecRtSweepCounts {
  size_t recycled;
  size_t retained;
  size_t retainedBytes;
};
void weavecRtSweep(void);
void weavecRtLastSweep(struct weavecRtSweepCounts *out);
extern unsigned long long weavecRtSweepNanos;
extern unsigned long long weavecRtSweepLongest;
/* RFC 0036 section 3.3: a sweep that ended at `ended` (CLOCK_MONOTONIC
 * nanoseconds) after `took`, and `marked` the heap or not, paces the next. */
void weavecRtSwept(uint64_t ended, uint64_t took, int marked);
/* The most bytes the live heap's slots and the quarantine held, for
 * WEAVEC_RT_STATS=1 (not synchronized). */
void weavecRtPeaks(size_t *live, size_t *quarantine);
void weavecRtSweepLock(void);
void weavecRtSweepUnlock(void);
void weavecRtLockAll(void);
void weavecRtUnlockAll(void);
int weavecRtSweepWanted(void);
/* RFC 0037 section 2.1: moves the calling thread's cached slots to the
 * shared lists, as a sweep does first. */
void weavecRtFlushThread(void);
/* Whether the calling thread is inside the allocator twice (a signal
 * handler interrupted it): such a call sweeps nothing. */
int weavecRtAllocatorNested(void);
int weavecRtPrepareMarks(void);
void weavecRtMarkWords(uintptr_t low, uintptr_t high);
void weavecRtMarkHeap(void);
/* RFC 0037 section 3: the live heap's marking in parts, for the mark
 * workers. weavecRtHeapWork calls `add` for runs of slots of about `chunk`
 * bytes; weavecRtMarkSlots marks from the live ones; weavecRtMarkHuge from
 * the live huge blocks. */
void weavecRtHeapWork(void (*add)(unsigned region, uint32_t first,
                                  uint32_t last),
                      size_t chunk);
void weavecRtMarkSlots(unsigned region, uint32_t first, uint32_t last);
void weavecRtMarkHuge(void);
/* RFC 0040 section 7: the resident bytes of writable memory the runtime
 * does not track (WEAVEC_RT_STATS=1). */
size_t weavecRtUntrackedBytes(void);
/* The bytes of live heap slots (approximate). */
size_t weavecRtLiveBytes(void);
/* A fork's child: it has no mark workers. */
void weavecRtSweepForkChild(void);
/* Moves what nothing marked to the free lists (unless `recycle` is 0), and
 * clears the marks. */
void weavecRtRecycle(struct weavecRtSweepCounts *counts, int recycle);

/* WEAVEC_RT_STATS=1: what the runtime did, printed when the program exits. */
enum {
  WeavecRtStatAllocations,
  WeavecRtStatReleases,
  WeavecRtStatRecycled,
  WeavecRtStatHuge,
  WeavecRtStatSlowGuards,
  WeavecRtStatRanges,
  WeavecRtStatStrings,
  WeavecRtStatStackUnpoisons,
  WeavecRtStatSweeps,
  WeavecRtStatCount
};
extern unsigned long long weavecRtStats[WeavecRtStatCount];
/* Whether WEAVEC_RT_STATS=1: the counters are written only then, so that
 * threads do not share their cache line (RFC 0037 section 2.2); sweeps are
 * always counted. */
extern int weavecRtStatsOn;
static inline void weavecRtCount(int stat) {
  if (weavecRtStatsOn || stat == WeavecRtStatSweeps)
    ++weavecRtStats[stat];
}

/* A lock that needs no initialization and nothing from the C library. */
typedef struct {
  volatile int held;
} WeavecRtLock;

static inline void weavecRtLock(WeavecRtLock *lock) {
  unsigned spins = 0;
  while (__atomic_exchange_n(&lock->held, 1, __ATOMIC_ACQUIRE) != 0) {
    while (__atomic_load_n(&lock->held, __ATOMIC_RELAXED) != 0) {
      if (++spins < 64) {
#if defined(__x86_64__) || defined(__i386__)
        __builtin_ia32_pause();
#elif defined(__aarch64__)
        __asm__ __volatile__("yield");
#endif
      } else {
        extern int sched_yield(void);
        (void)sched_yield();
      }
    }
  }
}

static inline void weavecRtUnlock(WeavecRtLock *lock) {
  __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE);
}

/* RFC 0040 section 1: the runtime's code in a section of its own, which the
 * unwinder recognizes (not the runtime's test, which includes this). */
#if !defined(WEAVEC_RT_TEST)
#if defined(__APPLE__)
#pragma clang section text = "__TEXT,__weavec_rt,regular,pure_instructions"
#else
#pragma clang section text = "weavec_rt_text"
#endif
#endif

#endif /* WEAVEC_RUNTIME_WEAVEC_RT_H */
