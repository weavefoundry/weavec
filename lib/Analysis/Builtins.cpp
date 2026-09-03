//===- Builtins.cpp - Shipped summaries for the C standard library --------===//
//
// Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
// See LICENSE for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// RFC 0003, provider step 3: summaries for the ISO C `<stdlib.h>`,
// `<string.h>` and `<stdio.h>` functions that take or return pointers, so
// calling libc from checked code neither warns nor hides a bug. Entries are
// matched by global name, as RFC 0002 matched its allocator list, and the
// list subsumes it. RFC 0004 (*The library table*) extends it to the POSIX
// and common GNU/BSD functions real programs call, so that `--strict-externs`
// is usable and the default boundary warning does not fire on `getline`.
//
// Each entry is a compact spec:
//
//   params  one character per parameter
//             'r'  reads through the pointer (shared borrow for the call)
//             'w'  writes through the pointer (mutable borrow for the call)
//             'f'  releases the pointer (`free`, `fclose`)
//             'm'  moves the pointer to the callee (`realloc`'s argument)
//             '.'  not a pointer, or no ownership effect
//   result  'F'  fresh owned allocation
//           '0'..'9'  a pointer *into* that argument (`strchr` returns into
//                `s`): an interior copy, RFC 0006
//           'A'..'J'  that argument itself (`memcpy` returns `dest`): an
//                exact copy of argument 0..9
//           '-'  not a pointer, or nothing known (`getenv` returns static
//                storage, which this RFC spells as unknown)
//
// The few functions whose behaviour the spec cannot express (`realloc`,
// `strtol`'s end pointer, `getline`'s buffer) are patched by hand below.
//
// Each entry describes ISO C / POSIX behaviour, not a platform extension.
// Callback arguments (`qsort`'s comparator, `pthread_create`'s start
// routine) are '.': the function pointer itself is not dereferenced in the
// ownership sense, and what the callee does with the data pointer is
// out of scope (RFC 0001, threads). A '.' on a pointer argument is
// load-bearing for RFC 0007: the caller treats the value as *escaped* (the
// library may retain it), so `putenv`'s string and `setvbuf`'s buffer are
// '.' rather than 'r'/'w'.
//
// Every 'F', 'f' and 'm' carries a release family (RFC 0007), stamped in
// `buildTable` from the member list there.
//
//===----------------------------------------------------------------------===//

#include "weavec/Analysis/Summaries.h"

#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

#include <array>
#include <cassert>
#include <set>
#include <utility>

using namespace clang;

namespace weavec::analysis {

namespace {

struct BuiltinSpec {
  llvm::StringLiteral name;
  llvm::StringLiteral params;
  char result;

  // The constructor keeps the table below terse; designated initialisers
  // would triple its width.
  constexpr BuiltinSpec(llvm::StringLiteral fn, llvm::StringLiteral args,
                        char ret)
      : name(fn), params(args), result(ret) {}
};

} // namespace

// clang-format off
static constexpr auto Specs = std::to_array<BuiltinSpec>({
    // -- ISO C (RFC 0003) -----------------------------------------------------
    // <stdlib.h>
    {"malloc",        ".",      'F'},
    {"calloc",        "..",     'F'},
    {"realloc",       "m.",     'F'},
    {"aligned_alloc", "..",     'F'},
    {"free",          "f",      '-'},
    {"strdup",        "r",      'F'},
    {"strndup",       "r.",     'F'},
    {"atoi",          "r",      '-'},
    {"atol",          "r",      '-'},
    {"atoll",         "r",      '-'},
    {"atof",          "r",      '-'},
    {"strtol",        "rw.",    '-'},
    {"strtoll",       "rw.",    '-'},
    {"strtoul",       "rw.",    '-'},
    {"strtoull",      "rw.",    '-'},
    {"strtod",        "rw",     '-'},
    {"strtof",        "rw",     '-'},
    {"strtold",       "rw",     '-'},
    {"getenv",        "r",      '-'},
    {"system",        "r",      '-'},
    {"qsort",         "w...",   '-'},
    {"bsearch",       "rr...",  '1'},
    // <string.h>
    {"memcpy",        "wr.",    'A'},
    {"memmove",       "wr.",    'A'},
    {"memset",        "w..",    'A'},
    {"memcmp",        "rr.",    '-'},
    {"memchr",        "r..",    '0'},
    {"strcpy",        "wr",     'A'},
    {"strncpy",       "wr.",    'A'},
    {"strcat",        "wr",     'A'},
    {"strncat",       "wr.",    'A'},
    {"strcmp",        "rr",     '-'},
    {"strncmp",       "rr.",    '-'},
    {"strcoll",       "rr",     '-'},
    {"strxfrm",       "wr.",    '-'},
    {"strchr",        "r.",     '0'},
    {"strrchr",       "r.",     '0'},
    {"strstr",        "rr",     '0'},
    {"strpbrk",       "rr",     '0'},
    {"strspn",        "rr",     '-'},
    {"strcspn",       "rr",     '-'},
    {"strlen",        "r",      '-'},
    {"strnlen",       "r.",     '-'},
    {"strtok",        "wr",     '0'},
    {"strerror",      ".",      '-'},
    // <stdio.h>
    {"fopen",         "rr",     'F'},
    {"fdopen",        ".r",     'F'},
    {"tmpfile",       "",       'F'},
    {"fclose",        "f",      '-'},
    {"fflush",        "w",      '-'},
    {"printf",        "r",      '-'},
    {"fprintf",       "wr",     '-'},
    {"sprintf",       "wr",     '-'},
    {"snprintf",      "w.r",    '-'},
    {"vprintf",       "r.",     '-'},
    {"vfprintf",      "wr.",    '-'},
    {"vsprintf",      "wr.",    '-'},
    {"vsnprintf",     "w.r.",   '-'},
    {"scanf",         "r",      '-'},
    {"fscanf",        "wr",     '-'},
    {"sscanf",        "rr",     '-'},
    {"puts",          "r",      '-'},
    {"fputs",         "rw",     '-'},
    {"fputc",         ".w",     '-'},
    {"putc",          ".w",     '-'},
    {"fgetc",         "w",      '-'},
    {"getc",          "w",      '-'},
    {"ungetc",        ".w",     '-'},
    {"fgets",         "w.w",    'A'},
    {"fread",         "w..w",   '-'},
    {"fwrite",        "r..w",   '-'},
    {"fseek",         "w..",    '-'},
    {"ftell",         "r",      '-'},
    {"rewind",        "w",      '-'},
    {"fgetpos",       "rw",     '-'},
    {"fsetpos",       "wr",     '-'},
    {"feof",          "r",      '-'},
    {"ferror",        "r",      '-'},
    {"clearerr",      "w",      '-'},
    {"perror",        "r",      '-'},
    {"remove",        "r",      '-'},
    {"rename",        "rr",     '-'},
    {"setvbuf",       "w...",   '-'},  // keeps the buffer: no effect, escapes
    {"setbuf",        "ww",     '-'},
    {"fileno",        "r",      '-'},

    // -- ISO C, remaining pointer-taking functions (RFC 0004) -----------------
    // <stdlib.h>
    {"abort",         "",       '-'},
    {"exit",          ".",      '-'},
    {"_Exit",         ".",      '-'},
    {"quick_exit",    ".",      '-'},
    {"atexit",        ".",      '-'},
    {"at_quick_exit", ".",      '-'},
    {"mblen",         "r.",     '-'},
    {"mbtowc",        "wr.",    '-'},
    {"wctomb",        "w.",     '-'},
    {"mbstowcs",      "wr.",    '-'},
    {"wcstombs",      "wr.",    '-'},
    // <string.h>
    {"strerror_r",    ".w.",    '-'},
    // <stdio.h>
    {"freopen",       "rrw",    'C'},
    {"tmpnam",        "w",      'A'},
    {"gets",          "w",      'A'},
    {"getchar",       "",       '-'},
    {"putchar",       ".",      '-'},
    {"vscanf",        "r.",     '-'},
    {"vfscanf",       "wr.",    '-'},
    {"vsscanf",       "rr.",    '-'},
    // <time.h>
    {"time",          "w",      '-'},
    {"clock",         "",       '-'},
    {"difftime",      "..",     '-'},
    {"mktime",        "w",      '-'},
    {"asctime",       "r",      '-'},
    {"ctime",         "r",      '-'},
    {"gmtime",        "r",      '-'},
    {"localtime",     "r",      '-'},
    {"strftime",      "w.rr",   '-'},
    {"timespec_get",  "w.",     '-'},
    // <wchar.h>
    {"wcslen",        "r",      '-'},
    {"wcscpy",        "wr",     'A'},
    {"wcsncpy",       "wr.",    'A'},
    {"wcscat",        "wr",     'A'},
    {"wcsncat",       "wr.",    'A'},
    {"wcscmp",        "rr",     '-'},
    {"wcsncmp",       "rr.",    '-'},
    {"wcschr",        "r.",     '0'},
    {"wcsrchr",       "r.",     '0'},
    {"wcsstr",        "rr",     '0'},
    {"wmemcpy",       "wr.",    'A'},
    {"wmemmove",      "wr.",    'A'},
    {"wmemset",       "w..",    'A'},
    {"wmemcmp",       "rr.",    '-'},
    {"wmemchr",       "r..",    '0'},
    {"wcstok",        "wrw",    '0'},
    // <setjmp.h>, <signal.h>
    {"longjmp",       "w.",     '-'},
    {"signal",        "..",     '-'},
    {"raise",         ".",      '-'},

    // -- POSIX and common GNU/BSD (RFC 0004) ----------------------------------
    // <stdlib.h> extensions
    {"posix_memalign", "w..",   '-'},
    {"reallocarray",  "m..",    'F'},
    {"realpath",      "rw",     'B'},
    {"mkstemp",       "w",      '-'},
    {"mkostemp",      "w.",     '-'},
    {"mkdtemp",       "w",      'A'},
    {"mktemp",        "w",      'A'},
    {"setenv",        "rr.",    '-'},
    {"unsetenv",      "r",      '-'},
    {"putenv",        ".",      '-'},  // keeps the string: no effect, escapes
    {"clearenv",      "",       '-'},
    {"qsort_r",       "w....",  '-'},
    {"random",        "",       '-'},
    {"srandom",       ".",      '-'},
    {"getsubopt",     "wrw",    '-'},
    {"a64l",          "r",      '-'},
    {"l64a",          ".",      '-'},
    // <stdio.h> extensions
    {"getline",       "www",    '-'},
    {"getdelim",      "ww.w",   '-'},
    {"asprintf",      "wr",     '-'},
    {"vasprintf",     "wr.",    '-'},
    {"dprintf",       ".r",     '-'},
    {"vdprintf",      ".r.",    '-'},
    {"popen",         "rr",     'F'},
    {"pclose",        "f",      '-'},
    {"fmemopen",      "w.r",    'F'},
    {"open_memstream", "ww",    'F'},
    {"fileno_unlocked", "r",    '-'},
    {"flockfile",     "w",      '-'},
    {"funlockfile",   "w",      '-'},
    {"ftrylockfile",  "w",      '-'},
    {"getc_unlocked", "w",      '-'},
    {"putc_unlocked", ".w",     '-'},
    {"fseeko",        "w..",    '-'},
    {"ftello",        "r",      '-'},
    {"fpurge",        "w",      '-'},
    {"ctermid",       "w",      'A'},
    {"tempnam",       "rr",     'F'},
    {"renameat",      ".r.r",   '-'},
    // <string.h> / <strings.h> extensions
    {"strtok_r",      "wrw",    '0'},
    {"strsep",        "wr",     '-'},
    {"stpcpy",        "wr",     '0'},
    {"stpncpy",       "wr.",    '0'},
    {"strcasestr",    "rr",     '0'},
    {"strchrnul",     "r.",     '0'},
    {"memmem",        "r.r.",   '0'},
    {"mempcpy",       "wr.",    '0'},
    {"memrchr",       "r..",    '0'},
    {"memccpy",       "wr..",   '0'},
    {"strlcpy",       "wr.",    '-'},
    {"strlcat",       "wr.",    '-'},
    {"strcasecmp",    "rr",     '-'},
    {"strncasecmp",   "rr.",    '-'},
    {"strcoll_l",     "rr.",    '-'},
    {"strsignal",     ".",      '-'},
    {"explicit_bzero", "w.",    '-'},
    {"bzero",         "w.",     '-'},
    {"bcopy",         "rw.",    '-'},
    {"bcmp",          "rr.",    '-'},
    {"index",         "r.",     '0'},
    {"rindex",        "r.",     '0'},
    {"ffs",           ".",      '-'},
    // <unistd.h>
    {"read",          ".w.",    '-'},
    {"write",         ".r.",    '-'},
    {"pread",         ".w..",   '-'},
    {"pwrite",        ".r..",   '-'},
    {"close",         ".",      '-'},
    {"pipe",          "w",      '-'},
    {"pipe2",         "w.",     '-'},
    {"dup",           ".",      '-'},
    {"dup2",          "..",     '-'},
    {"dup3",          "...",    '-'},
    {"lseek",         "...",    '-'},
    {"access",        "r.",     '-'},
    {"faccessat",     ".r..",   '-'},
    {"unlink",        "r",      '-'},
    {"unlinkat",      ".r.",    '-'},
    {"rmdir",         "r",      '-'},
    {"chdir",         "r",      '-'},
    {"fchdir",        ".",      '-'},
    {"getcwd",        "w.",     'A'},
    {"readlink",      "rw.",    '-'},
    {"readlinkat",    ".rw.",   '-'},
    {"symlink",       "rr",     '-'},
    {"symlinkat",     "r.r",    '-'},
    {"link",          "rr",     '-'},
    {"linkat",        ".r.r.",  '-'},
    {"chown",         "r..",    '-'},
    {"fchown",        "...",    '-'},
    {"lchown",        "r..",    '-'},
    {"truncate",      "r.",     '-'},
    {"ftruncate",     "..",     '-'},
    {"fsync",         ".",      '-'},
    {"fdatasync",     ".",      '-'},
    {"isatty",        ".",      '-'},
    {"ttyname",       ".",      '-'},
    {"ttyname_r",     ".w.",    '-'},
    {"gethostname",   "w.",     '-'},
    {"sethostname",   "r.",     '-'},
    {"getlogin",      "",       '-'},
    {"getlogin_r",    "w.",     '-'},
    {"getopt",        ".rr",    '-'},
    {"execv",         "rr",     '-'},
    {"execve",        "rrr",    '-'},
    {"execvp",        "rr",     '-'},
    {"execl",         "rr",     '-'},
    {"execlp",        "rr",     '-'},
    {"fexecve",       ".rr",    '-'},
    {"fork",          "",       '-'},
    {"_exit",         ".",      '-'},
    {"alarm",         ".",      '-'},
    {"pause",         "",       '-'},
    {"sleep",         ".",      '-'},
    {"usleep",        ".",      '-'},
    {"getpid",        "",       '-'},
    {"getppid",       "",       '-'},
    {"sysconf",       ".",      '-'},
    {"pathconf",      "r.",     '-'},
    {"fpathconf",     "..",     '-'},
    {"chroot",        "r",      '-'},
    {"nice",          ".",      '-'},
    {"sync",          "",       '-'},
    {"getentropy",    "w.",     '-'},
    // <fcntl.h>
    {"open",          "r.",     '-'},
    {"openat",        ".r.",    '-'},
    {"creat",         "r.",     '-'},
    {"fcntl",         "..",     '-'},
    {"posix_fadvise", "....",   '-'},
    {"posix_fallocate", "...",  '-'},
    // <sys/stat.h>
    {"stat",          "rw",     '-'},
    {"fstat",         ".w",     '-'},
    {"lstat",         "rw",     '-'},
    {"fstatat",       ".rw.",   '-'},
    {"chmod",         "r.",     '-'},
    {"fchmod",        "..",     '-'},
    {"fchmodat",      ".r..",   '-'},
    {"mkdir",         "r.",     '-'},
    {"mkdirat",       ".r.",    '-'},
    {"mkfifo",        "r.",     '-'},
    {"mknod",         "r..",    '-'},
    {"umask",         ".",      '-'},
    {"utimensat",     ".rr.",   '-'},
    {"futimens",      ".r",     '-'},
    // <dirent.h>
    {"opendir",       "r",      'F'},
    {"fdopendir",     ".",      'F'},
    {"closedir",      "f",      '-'},
    // `readdir`'s entry lives in the stream: it is "the same object" as
    // `dirp` for the purposes of `closedir`, which is what a copy says.
    {"readdir",       "w",      '0'},
    {"readdir_r",     "www",    '-'},
    {"rewinddir",     "w",      '-'},
    {"seekdir",       "w.",     '-'},
    {"telldir",       "r",      '-'},
    {"dirfd",         "r",      '-'},
    {"scandir",       "rw..",   '-'},
    {"alphasort",     "rr",     '-'},
    // <time.h> extensions
    {"localtime_r",   "rw",     'B'},
    {"gmtime_r",      "rw",     'B'},
    {"asctime_r",     "rw",     'B'},
    {"ctime_r",       "rw",     'B'},
    {"strptime",      "rrw",    '-'},
    {"clock_gettime", ".w",     '-'},
    {"clock_getres",  ".w",     '-'},
    {"clock_settime", ".r",     '-'},
    {"clock_nanosleep", "..rw", '-'},
    {"nanosleep",     "rw",     '-'},
    {"timegm",        "w",      '-'},
    {"tzset",         "",       '-'},
    {"timer_create",  ".rw",    '-'},
    {"timer_settime", "..rw",   '-'},
    {"timer_gettime", ".w",     '-'},
    {"timer_delete",  ".",      '-'},
    // <sys/time.h>
    {"gettimeofday",  "ww",     '-'},
    {"settimeofday",  "rr",     '-'},
    {"utimes",        "rr",     '-'},
    {"futimes",       ".r",     '-'},
    {"getitimer",     ".w",     '-'},
    {"setitimer",     ".rw",    '-'},
    // <sys/mman.h>
    {"mmap",          "r.....", 'F'},
    {"munmap",        "f.",     '-'},
    {"mprotect",      "w..",    '-'},
    {"msync",         "w..",    '-'},
    {"madvise",       "w..",    '-'},
    {"mlock",         "r.",     '-'},
    {"munlock",       "r.",     '-'},
    {"shm_open",      "r..",    '-'},
    {"shm_unlink",    "r",      '-'},
    // <pthread.h>
    {"pthread_create", "wr..",  '-'},
    {"pthread_join",  ".w",     '-'},
    {"pthread_detach", ".",     '-'},
    {"pthread_exit",  ".",      '-'},
    {"pthread_self",  "",       '-'},
    {"pthread_equal", "..",     '-'},
    {"pthread_cancel", ".",     '-'},
    {"pthread_attr_init", "w",  '-'},
    {"pthread_attr_destroy", "w", '-'},
    {"pthread_attr_setdetachstate", "w.", '-'},
    {"pthread_attr_setstacksize", "w.", '-'},
    {"pthread_mutex_init", "wr", '-'},
    {"pthread_mutex_destroy", "w", '-'},
    {"pthread_mutex_lock", "w", '-'},
    {"pthread_mutex_trylock", "w", '-'},
    {"pthread_mutex_unlock", "w", '-'},
    {"pthread_mutexattr_init", "w", '-'},
    {"pthread_mutexattr_destroy", "w", '-'},
    {"pthread_mutexattr_settype", "w.", '-'},
    {"pthread_cond_init", "wr", '-'},
    {"pthread_cond_destroy", "w", '-'},
    {"pthread_cond_wait", "ww", '-'},
    {"pthread_cond_timedwait", "wwr", '-'},
    {"pthread_cond_signal", "w", '-'},
    {"pthread_cond_broadcast", "w", '-'},
    {"pthread_rwlock_init", "wr", '-'},
    {"pthread_rwlock_destroy", "w", '-'},
    {"pthread_rwlock_rdlock", "w", '-'},
    {"pthread_rwlock_wrlock", "w", '-'},
    {"pthread_rwlock_unlock", "w", '-'},
    {"pthread_spin_init", "w.", '-'},
    {"pthread_spin_destroy", "w", '-'},
    {"pthread_spin_lock", "w",  '-'},
    {"pthread_spin_unlock", "w", '-'},
    {"pthread_once",  "w.",     '-'},
    {"pthread_key_create", "w.", '-'},
    {"pthread_key_delete", ".", '-'},
    {"pthread_setspecific", "..", '-'},
    {"pthread_getspecific", ".", '-'},
    {"pthread_sigmask", ".rw",  '-'},
    {"pthread_kill",  "..",     '-'},
    {"pthread_setname_np", ".r", '-'},
    // <sys/socket.h>, <netdb.h>, <arpa/inet.h>, <netinet/in.h>
    {"socket",        "...",    '-'},
    {"socketpair",    "...w",   '-'},
    {"bind",          ".r.",    '-'},
    {"listen",        "..",     '-'},
    {"accept",        ".ww",    '-'},
    {"accept4",       ".ww.",   '-'},
    {"connect",       ".r.",    '-'},
    {"shutdown",      "..",     '-'},
    {"send",          ".r..",   '-'},
    {"sendto",        ".r..r.", '-'},
    {"sendmsg",       ".r.",    '-'},
    {"recv",          ".w..",   '-'},
    {"recvfrom",      ".w..ww", '-'},
    {"recvmsg",       ".w.",    '-'},
    {"getsockopt",    "...ww",  '-'},
    {"setsockopt",    "...r.",  '-'},
    {"getsockname",   ".ww",    '-'},
    {"getpeername",   ".ww",    '-'},
    {"getaddrinfo",   "rrrw",   '-'},
    {"freeaddrinfo",  "f",      '-'},
    {"gai_strerror",  ".",      '-'},
    {"getnameinfo",   "r.w.w..", '-'},
    {"gethostbyname", "r",      '-'},
    {"gethostbyaddr", "r..",    '-'},
    {"getservbyname", "rr",     '-'},
    {"getservbyport", ".r",     '-'},
    {"getprotobyname", "r",     '-'},
    {"inet_ntop",     ".rw.",   'C'},
    {"inet_pton",     ".rw",    '-'},
    {"inet_addr",     "r",      '-'},
    {"inet_aton",     "rw",     '-'},
    {"inet_ntoa",     ".",      '-'},
    {"htons",         ".",      '-'},
    {"htonl",         ".",      '-'},
    {"ntohs",         ".",      '-'},
    {"ntohl",         ".",      '-'},
    // <sys/uio.h>, <poll.h>, <sys/select.h>, <sys/epoll.h>, <sys/event.h>
    {"readv",         ".r.",    '-'},
    {"writev",        ".r.",    '-'},
    {"preadv",        ".r..",   '-'},
    {"pwritev",       ".r..",   '-'},
    {"poll",          "w..",    '-'},
    {"ppoll",         "w.rr",   '-'},
    {"select",        ".wwww",  '-'},
    {"pselect",       ".wwwrr", '-'},
    {"epoll_create",  ".",      '-'},
    {"epoll_create1", ".",      '-'},
    {"epoll_ctl",     "...r",   '-'},
    {"epoll_wait",    ".w..",   '-'},
    {"kqueue",        "",       '-'},
    {"kevent",        ".r.w.r", '-'},
    // <signal.h>, <sys/wait.h>
    {"sigaction",     ".rw",    '-'},
    {"sigemptyset",   "w",      '-'},
    {"sigfillset",    "w",      '-'},
    {"sigaddset",     "w.",     '-'},
    {"sigdelset",     "w.",     '-'},
    {"sigismember",   "r.",     '-'},
    {"sigprocmask",   ".rw",    '-'},
    {"sigsuspend",    "r",      '-'},
    {"sigwait",       "rw",     '-'},
    {"sigpending",    "w",      '-'},
    {"kill",          "..",     '-'},
    {"killpg",        "..",     '-'},
    {"wait",          "w",      '-'},
    {"waitpid",       ".w.",    '-'},
    {"waitid",        "..w.",   '-'},
    // <dlfcn.h>
    {"dlopen",        "r.",     'F'},
    {"dlclose",       "f",      '-'},
    {"dlsym",         "rr",     '-'},
    {"dlerror",       "",       '-'},
    {"dladdr",        "rw",     '-'},
    // <regex.h>, <fnmatch.h>, <glob.h>
    {"regcomp",       "wr.",    '-'},
    {"regexec",       "rr.w.",  '-'},
    {"regerror",      ".rw.",   '-'},
    {"regfree",       "w",      '-'},
    {"fnmatch",       "rr.",    '-'},
    {"glob",          "r..w",   '-'},
    {"globfree",      "w",      '-'},
    // <err.h>, <syslog.h>, <libgen.h>, <errno.h>
    {"err",           ".r",     '-'},
    {"errx",          ".r",     '-'},
    {"warn",          "r",      '-'},
    {"warnx",         "r",      '-'},
    {"verr",          ".r.",    '-'},
    {"verrx",         ".r.",    '-'},
    {"vwarn",         "r.",     '-'},
    {"vwarnx",        "r.",     '-'},
    {"openlog",       "r..",    '-'},
    {"syslog",        ".r",     '-'},
    {"vsyslog",       ".r.",    '-'},
    {"closelog",      "",       '-'},
    {"setlogmask",    ".",      '-'},
    {"basename",      "w",      '-'},
    {"dirname",       "w",      '-'},
    {"__errno_location", "",    '-'},
    {"__error",       "",       '-'},
    // <pwd.h>, <grp.h>: the `_r` forms fill the caller's buffer and store a
    // borrow of it through the result pointer; the others are static storage.
    {"getpwnam",      "r",      '-'},
    {"getpwuid",      ".",      '-'},
    {"getpwnam_r",    "rww.w",  '-'},
    {"getpwuid_r",    ".ww.w",  '-'},
    {"getgrnam",      "r",      '-'},
    {"getgrgid",      ".",      '-'},
    {"getgrnam_r",    "rww.w",  '-'},
    {"getgrgid_r",    ".ww.w",  '-'},
    {"getgroups",     ".w",     '-'},
    {"setgroups",     ".r",     '-'},
    // <iconv.h>, <locale.h>, <langinfo.h>
    {"iconv_open",    "rr",     'F'},
    {"iconv_close",   "f",      '-'},
    {"iconv",         "wwwww",  '-'},
    {"setlocale",     ".r",     '-'},
    {"localeconv",    "",       '-'},
    {"newlocale",     ".rm",    'F'},
    {"freelocale",    "f",      '-'},
    {"uselocale",     ".",      '-'},
    {"nl_langinfo",   ".",      '-'},
    // <sys/utsname.h>, <sys/resource.h>, <sys/ioctl.h>, <termios.h>
    {"uname",         "w",      '-'},
    {"getrlimit",     ".w",     '-'},
    {"setrlimit",     ".r",     '-'},
    {"getrusage",     ".w",     '-'},
    {"ioctl",         "..",     '-'},
    {"tcgetattr",     ".w",     '-'},
    {"tcsetattr",     "..r",    '-'},
    {"cfmakeraw",     "w",      '-'},
    // <ctype.h>-adjacent and misc GNU/BSD helpers with pointer arguments
    {"strtoimax",     "rw.",    '-'},
    {"strtoumax",     "rw.",    '-'},
    {"getprogname",   "",       '-'},
    {"setprogname",   "r",      '-'},
    {"arc4random_buf", "w.",    '-'},
    {"getrandom",     "w..",    '-'},
    {"backtrace",     "w.",     '-'},
    {"backtrace_symbols", "r.", 'F'},
    {"backtrace_symbols_fd", "r..", '-'},
});
// clang-format on

static core::FunctionSummary fromSpec(const BuiltinSpec &spec) {
  core::FunctionSummary summary;
  for (unsigned i = 0; i < spec.params.size(); ++i) {
    const core::SummaryPath root = core::SummaryPath::param(i);
    switch (spec.params[i]) {
    case 'r':
      summary.addEffect(root.deref(), core::PlaceEffect{.read = true});
      break;
    case 'w':
      summary.addEffect(root.deref(), core::PlaceEffect{.written = true});
      break;
    case 'f':
      summary.addEffect(root, core::PlaceEffect{.freed = true});
      break;
    case 'm':
      summary.addEffect(root, core::PlaceEffect{.moved = true});
      break;
    case '.':
      break;
    default:
      assert(false && "unknown builtin parameter spec");
    }
  }
  if (spec.result == 'F') {
    summary.addReturn(core::ValueSource::fresh());
  } else if (spec.result >= '0' && spec.result <= '9') {
    summary.addReturn(core::ValueSource::interiorCopy(
        core::SummaryPath::param(static_cast<unsigned>(spec.result - '0'))));
  } else if (spec.result >= 'A' && spec.result <= 'J') {
    summary.addReturn(core::ValueSource::copy(
        core::SummaryPath::param(static_cast<unsigned>(spec.result - 'A'))));
  }
  return summary;
}

static llvm::StringMap<core::FunctionSummary> buildTable() {
  llvm::StringMap<core::FunctionSummary> table;
  for (const BuiltinSpec &spec : Specs)
    table[spec.name] = fromSpec(spec);

  // `realloc(p, n)` consumes `p` only when it succeeds (RFC 0006,
  // *Outcome-conditional summaries*, replacing RFC 0002's `realloc-like`).
  for (const llvm::StringLiteral name :
       {llvm::StringLiteral("realloc"), llvm::StringLiteral("reallocarray")}) {
    core::FunctionSummary &summary = table[name];
    summary.addReturn(core::ValueSource::null());
    summary.addOutcome(core::Outcome::Null);
    summary.addOutcome(core::Outcome::NonNull, core::SummaryPath::param(0),
                       core::PlaceEffect{.moved = true});
  }

  // `_FORTIFY_SOURCE` rewrites `memcpy(d, s, n)` into
  // `__builtin___memcpy_chk(d, s, n, __builtin_object_size(d, 0))`; the
  // checked form has the plain one's effects (the extra trailing argument
  // is a size). The `*printf_chk` forms insert their extra arguments in
  // the middle and are left to the intrinsic rule.
  for (const llvm::StringLiteral name :
       {llvm::StringLiteral("memcpy"), llvm::StringLiteral("memmove"),
        llvm::StringLiteral("memset"), llvm::StringLiteral("strcpy"),
        llvm::StringLiteral("strncpy"), llvm::StringLiteral("strcat"),
        llvm::StringLiteral("strncat"), llvm::StringLiteral("stpcpy"),
        llvm::StringLiteral("stpncpy"), llvm::StringLiteral("strlcpy"),
        llvm::StringLiteral("strlcat")}) {
    if (const auto it = table.find(name); it != table.end())
      table[("__builtin___" + name + "_chk").str()] = it->second;
  }

  // `strto*(s, &end, ...)` store a pointer into `s` through `end`.
  for (const llvm::StringLiteral name :
       {llvm::StringLiteral("strtol"), llvm::StringLiteral("strtoll"),
        llvm::StringLiteral("strtoul"), llvm::StringLiteral("strtoull"),
        llvm::StringLiteral("strtod"), llvm::StringLiteral("strtof"),
        llvm::StringLiteral("strtold"), llvm::StringLiteral("strtoimax"),
        llvm::StringLiteral("strtoumax")}) {
    table[name].addStore(core::Store{
        .dest = core::SummaryPath::param(1).deref(),
        .value = core::ValueSource::interiorCopy(core::SummaryPath::param(0))});
  }

  // Out-parameters that receive a fresh allocation the caller must release
  // (RFC 0004, *The library table*): `getline(&buf, &n, f)`,
  // `asprintf(&s, ...)`, `posix_memalign(&p, ...)`, `getaddrinfo(..., &res)`,
  // `scandir(dir, &names, ...)`, `vasprintf`, `getdelim`.
  const auto storesFresh = [&table](llvm::StringRef name, unsigned param) {
    table[name].addStore(
        core::Store{.dest = core::SummaryPath::param(param).deref(),
                    .value = core::ValueSource::fresh()});
  };
  storesFresh("getline", 0);
  storesFresh("getdelim", 0);
  storesFresh("asprintf", 0);
  storesFresh("vasprintf", 0);
  storesFresh("posix_memalign", 0);
  storesFresh("getaddrinfo", 3);
  storesFresh("scandir", 1);

  // `getline`/`getdelim` reallocate the buffer they are given: the old
  // `*lineptr` is consumed before the fresh one is stored, so reusing the
  // buffer across iterations is not a leak (RFC 0007, *The library table*).
  for (const llvm::StringLiteral name :
       {llvm::StringLiteral("getline"), llvm::StringLiteral("getdelim")}) {
    table[name].addEffect(core::SummaryPath::param(0).deref(),
                          core::PlaceEffect{.moved = true});
  }

  // Release families (RFC 0007): every allocator, releaser and consuming
  // parameter in the table belongs to the family named after its releaser.
  // The releaser itself is a member so `free` is `freed(free)`.
  struct FamilyMember {
    llvm::StringLiteral member;
    llvm::StringLiteral family;

    // As for `BuiltinSpec`: the constructor keeps the table terse.
    constexpr FamilyMember(llvm::StringLiteral fn, llvm::StringLiteral of)
        : member(fn), family(of) {}
  };
  // clang-format off
  static constexpr auto Families = std::to_array<FamilyMember>({
      {"free", "free"},           {"malloc", "free"},
      {"calloc", "free"},         {"realloc", "free"},
      {"reallocarray", "free"},   {"aligned_alloc", "free"},
      {"strdup", "free"},         {"strndup", "free"},
      {"tempnam", "free"},        {"backtrace_symbols", "free"},
      {"getline", "free"},        {"getdelim", "free"},
      {"asprintf", "free"},       {"vasprintf", "free"},
      {"posix_memalign", "free"}, {"scandir", "free"},
      {"fclose", "fclose"},       {"fopen", "fclose"},
      {"fdopen", "fclose"},       {"tmpfile", "fclose"},
      {"fmemopen", "fclose"},     {"open_memstream", "fclose"},
      {"pclose", "pclose"},       {"popen", "pclose"},
      {"closedir", "closedir"},   {"opendir", "closedir"},
      {"fdopendir", "closedir"},
      {"munmap", "munmap"},       {"mmap", "munmap"},
      {"freeaddrinfo", "freeaddrinfo"}, {"getaddrinfo", "freeaddrinfo"},
      {"dlclose", "dlclose"},     {"dlopen", "dlclose"},
      {"iconv_close", "iconv_close"}, {"iconv_open", "iconv_close"},
      {"freelocale", "freelocale"}, {"newlocale", "freelocale"},
  });
  // clang-format on
  const auto stamp = [&table](llvm::StringRef member, llvm::StringRef family) {
    const auto it = table.find(member);
    assert(it != table.end() && "family member missing from the table");
    core::FunctionSummary &summary = it->second;
    for (auto &[path, effect] : summary.effects)
      if (effect.consumed())
        effect.family = family.str();
    for (auto &[outcome, effects] : summary.outcomes)
      for (auto &[path, effect] : effects)
        if (effect.consumed())
          effect.family = family.str();
    std::set<core::Store> stores;
    for (core::Store store : summary.stores) {
      if (store.value.isFresh())
        store.value.family = family.str();
      stores.insert(std::move(store));
    }
    summary.stores = std::move(stores);
    if (summary.returnsFresh()) {
      summary.eraseFreshReturns();
      summary.addReturn(core::ValueSource::fresh(family.str()));
    }
  };
  for (const FamilyMember &entry : Families)
    stamp(entry.member, entry.family);

  // `strtok_r(s, delim, &save)` parks a pointer into `s` in `*save`.
  table["strtok_r"].addStore(core::Store{
      .dest = core::SummaryPath::param(2).deref(),
      .value = core::ValueSource::interiorCopy(core::SummaryPath::param(0))});
  // The `_r` lookups store a borrow of the caller's record through `result`.
  for (const llvm::StringLiteral name :
       {llvm::StringLiteral("getpwnam_r"), llvm::StringLiteral("getpwuid_r"),
        llvm::StringLiteral("getgrnam_r"), llvm::StringLiteral("getgrgid_r")}) {
    table[name].addStore(core::Store{
        .dest = core::SummaryPath::param(4).deref(),
        .value = core::ValueSource::copy(core::SummaryPath::param(1))});
  }
  return table;
}

static const llvm::StringMap<core::FunctionSummary> &table() {
  static const llvm::StringMap<core::FunctionSummary> Table = buildTable();
  return Table;
}

const core::FunctionSummary *builtinSummary(const FunctionDecl &function) {
  // Only global functions count: a static helper called `free` in some file
  // is the user's business, not libc's.
  if (!function.isGlobal())
    return nullptr;
  const IdentifierInfo *ident = function.getIdentifier();
  if (ident == nullptr)
    return nullptr;
  const auto it = table().find(ident->getName());
  return it == table().end() ? nullptr : &it->second;
}

std::vector<llvm::StringRef> builtinNames() {
  std::vector<llvm::StringRef> names;
  names.reserve(Specs.size());
  for (const BuiltinSpec &spec : Specs)
    names.push_back(spec.name);
  return names;
}

} // namespace weavec::analysis
