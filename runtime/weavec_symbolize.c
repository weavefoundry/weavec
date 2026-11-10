/*===- weavec_symbolize.c - Naming a report's frames --------------*- C -*-===*\
|*
|* Part of WeaveC, under the Apache License v2.0 with LLVM Exceptions.
|* See LICENSE for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
|*===----------------------------------------------------------------------===*|
|*
|* RFC 0040, section 4. A report's frames are named by a symbolizer that
|* reads their modules' line tables: on Linux `weavec-cc --weavec-symbolize`
|* (LLVM's Symbolize library), found beside the runtime's library and kept
|* running; on Darwin `atos`, which reads the debug map that links an
|* executable to its objects' line tables, falling back to weavec-cc. Both
|* answer each address with lines `function` and `file:line[:column]` (more
|* for inlined calls) and a blank line. Without a symbolizer (none found, a
|* set-user-ID program, WEAVEC_RT_SYMBOLIZE=0, five seconds gone) a frame is
|* named by its module and offset and by the symbol `dladdr` finds.
|*
\*===----------------------------------------------------------------------===*/

#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include "weavec_rt.h"

#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#else
#include <link.h>
#include <sys/auxv.h>
#endif

extern char **environ;

/*===-- Settings ------------------------------------------------------------===*/

static int symbolizeOff;
/* WEAVEC_RT_SYMBOLIZER was given: Darwin asks it, not atos. */
static int symbolizerChosen;
/* WEAVEC_RT_SYMBOLIZER, or the weavec-cc beside the runtime's library. */
static char symbolizer[1024];
static char workDirectory[1024];

static void copyText(char *out, size_t room, const char *text) {
  const size_t length = strlen(text);
  if (length < room)
    memcpy(out, text, length + 1);
}

/* <prefix>/lib/weavec/libweavec_rt.* -> <prefix>/bin/weavec-cc. */
static void findDriver(void) {
  Dl_info own;
  char path[1024];
  unsigned up;
  char *slash = NULL;
  if (dladdr((const void *)findDriver, &own) == 0 || own.dli_fname == NULL)
    return;
  copyText(path, sizeof path, own.dli_fname);
  slash = strrchr(path, '/');
  if (slash == NULL || strncmp(slash + 1, "libweavec_rt.", 13) != 0)
    return; /* (a -static program: the runtime is in the program) */
  for (up = 0; up < 2 && slash != NULL; ++up) {
    *slash = 0;
    slash = strrchr(path, '/');
  }
  if (slash == NULL || (size_t)(slash - path) + 16 >= sizeof symbolizer)
    return;
  memcpy(slash, "/bin/weavec-cc", 15);
  copyText(symbolizer, sizeof symbolizer, path);
}

__attribute__((constructor)) static void readSymbolizeSettings(void) {
  const char *value = getenv("WEAVEC_RT_SYMBOLIZE");
#if defined(__APPLE__)
  const int secure = issetugid();
#else
  const int secure = getauxval(AT_SECURE) != 0;
#endif
  /* A set-user-ID or set-group-ID program runs nothing its environment
   * names. */
  symbolizeOff = secure || (value != NULL && strcmp(value, "0") == 0);
  value = getenv("WEAVEC_RT_SYMBOLIZER");
  symbolizerChosen = !secure && value != NULL && *value != 0;
  if (symbolizerChosen)
    copyText(symbolizer, sizeof symbolizer, value);
  else
    findDriver();
}

/*===-- Strings of one symbolization ----------------------------------------===*/

static char arena[65536];
static size_t arenaUsed;

static const char *keep(const char *text, size_t length) {
  char *out;
  if (arenaUsed + length + 1 > sizeof arena)
    return "?";
  out = arena + arenaUsed;
  memcpy(out, text, length);
  out[length] = 0;
  arenaUsed += length + 1;
  return out;
}

/*===-- Modules -------------------------------------------------------------===*/

/* The module that holds `pc`, the offset from its load address, the address
 * in the file the CODE protocol takes, and (Darwin) its load address. */
struct Module {
  const char *path;
  uintptr_t offset;
  uintptr_t address;
  uintptr_t header;
};

#if defined(__APPLE__)
/* A Mach-O image's addresses start at its __TEXT segment's (0x100000000 in
 * an executable). */
static uintptr_t textAddress(uintptr_t header) {
  const struct mach_header_64 *image = (const struct mach_header_64 *)header;
  const struct load_command *command = (const struct load_command *)(image + 1);
  uint32_t i;
  for (i = 0; i < image->ncmds; ++i) {
    const struct segment_command_64 *segment =
        (const struct segment_command_64 *)command;
    if (command->cmd == LC_SEGMENT_64 && strcmp(segment->segname, "__TEXT") == 0)
      return (uintptr_t)segment->vmaddr;
    command = (const struct load_command *)((const char *)command +
                                            command->cmdsize);
  }
  return 0;
}

static int moduleOf(uintptr_t pc, struct Module *out) {
  Dl_info info;
  if (dladdr((const void *)pc, &info) == 0 || info.dli_fname == NULL)
    return 0;
  out->path = keep(info.dli_fname, strlen(info.dli_fname));
  out->header = (uintptr_t)info.dli_fbase;
  out->offset = pc - out->header;
  out->address = out->offset + textAddress(out->header);
  return 1;
}
#else
struct Search {
  uintptr_t pc;
  const char *name;
  uintptr_t bias;
  int found;
};

static int findModule(struct dl_phdr_info *info, size_t size, void *data) {
  struct Search *search = (struct Search *)data;
  unsigned i;
  (void)size;
  for (i = 0; i < info->dlpi_phnum; ++i) {
    const ElfW(Phdr) *segment = &info->dlpi_phdr[i];
    const uintptr_t low = info->dlpi_addr + segment->p_vaddr;
    if (segment->p_type == PT_LOAD && search->pc >= low &&
        search->pc < low + segment->p_memsz) {
      search->name = info->dlpi_name;
      search->bias = info->dlpi_addr;
      search->found = 1;
      return 1;
    }
  }
  return 0;
}

static int moduleOf(uintptr_t pc, struct Module *out) {
  struct Search search = {pc, NULL, 0, 0};
  (void)dl_iterate_phdr(findModule, &search);
  if (!search.found)
    return 0;
  /* The program's own name is empty. */
  if (search.name == NULL || *search.name == 0) {
    char path[1024];
    const long length = (long)readlink("/proc/self/exe", path, sizeof path);
    if (length <= 0 || (size_t)length >= sizeof path)
      return 0;
    out->path = keep(path, (size_t)length);
  } else {
    out->path = keep(search.name, strlen(search.name));
  }
  out->offset = pc - search.bias;
  out->address = out->offset;
  out->header = search.bias;
  return 1;
}
#endif

/*===-- Children ------------------------------------------------------------===*/

struct Child {
  pid_t pid;
  int in;  /* its standard input */
  int out; /* its standard output */
  pid_t owner;
};

/* The weavec-cc kept running (Linux; Darwin's fallback). */
static struct Child kept = {0, -1, -1, 0};
static int keptFailed;

static uint64_t now(void) {
  struct timespec time;
  (void)clock_gettime(CLOCK_MONOTONIC, &time);
  return (uint64_t)time.tv_sec * 1000 + (uint64_t)time.tv_nsec / 1000000;
}

static void closeChild(struct Child *child, int force) {
  if (child->in >= 0)
    (void)close(child->in);
  if (child->out >= 0)
    (void)close(child->out);
  if (child->pid > 0 && child->owner == getpid()) {
    int status;
    if (force)
      (void)kill(child->pid, SIGKILL);
    (void)waitpid(child->pid, &status, 0);
  }
  child->pid = 0;
  child->in = child->out = -1;
}

static int spawn(struct Child *child, char *const argv[]) {
  int in[2];
  int out[2];
  posix_spawn_file_actions_t actions;
  int ok;
  if (pipe(in) != 0)
    return 0;
  if (pipe(out) != 0) {
    (void)close(in[0]), (void)close(in[1]);
    return 0;
  }
  (void)fcntl(in[1], F_SETFD, FD_CLOEXEC);
  (void)fcntl(out[0], F_SETFD, FD_CLOEXEC);
#if defined(F_SETNOSIGPIPE)
  (void)fcntl(in[1], F_SETNOSIGPIPE, 1);
#endif
  ok = posix_spawn_file_actions_init(&actions) == 0;
  if (ok) {
    (void)posix_spawn_file_actions_adddup2(&actions, in[0], 0);
    (void)posix_spawn_file_actions_adddup2(&actions, out[1], 1);
    (void)posix_spawn_file_actions_addopen(&actions, 2, "/dev/null", O_WRONLY,
                                           0);
    ok = posix_spawn(&child->pid, argv[0], &actions, NULL, argv, environ) == 0;
    (void)posix_spawn_file_actions_destroy(&actions);
  }
  (void)close(in[0]);
  (void)close(out[1]);
  if (!ok) {
    (void)close(in[1]), (void)close(out[0]);
    child->pid = 0;
    return 0;
  }
  child->in = in[1];
  child->out = out[0];
  child->owner = getpid();
  return 1;
}

/* Writes all of `text`; false when the child is gone. A write to a child
 * that died raises no SIGPIPE. */
static int writeAll(int fd, const char *text, size_t size) {
  sigset_t pipeSignal;
  sigset_t old;
  int ok = 1;
  sigemptyset(&pipeSignal);
  sigaddset(&pipeSignal, SIGPIPE);
  (void)pthread_sigmask(SIG_BLOCK, &pipeSignal, &old);
  while (size != 0) {
    const long written = (long)write(fd, text, size);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      ok = 0;
      break;
    }
    text += written;
    size -= (size_t)written;
  }
#if !defined(__APPLE__)
  if (!ok) {
    const struct timespec zero = {0, 0};
    (void)sigtimedwait(&pipeSignal, NULL, &zero);
  }
#endif
  (void)pthread_sigmask(SIG_SETMASK, &old, NULL);
  return ok;
}

/* Reads one line into `line` (without its newline) before `deadline`;
 * false at the end of the output or the deadline. */
struct Reader {
  int fd;
  char buffer[4096];
  size_t start;
  size_t end;
};

static int readLine(struct Reader *reader, char *line, size_t room,
                    uint64_t deadline) {
  size_t length = 0;
  for (;;) {
    while (reader->start < reader->end) {
      const char c = reader->buffer[reader->start++];
      if (c == '\n') {
        line[length] = 0;
        return 1;
      }
      if (length + 1 < room)
        line[length++] = c;
    }
    {
      struct pollfd wait = {reader->fd, POLLIN, 0};
      const uint64_t at = now();
      long got;
      if (at >= deadline || poll(&wait, 1, (int)(deadline - at)) <= 0)
        return 0;
      got = (long)read(reader->fd, reader->buffer, sizeof reader->buffer);
      if (got < 0 && errno == EINTR)
        continue;
      if (got <= 0)
        return 0;
      reader->start = 0;
      reader->end = (size_t)got;
    }
  }
}

/*===-- Answers -------------------------------------------------------------===*/

/* A path under the working directory, relative to it. */
static const char *shortPath(const char *path, size_t length) {
  const size_t work = strlen(workDirectory);
  if (work != 0 && length > work + 1 && strncmp(path, workDirectory, work) == 0 &&
      path[work] == '/')
    return keep(path + work + 1, length - work - 1);
  return keep(path, length);
}

/* `file:line[:column]` (llvm-symbolizer) into `symbol`. */
static void parseLocation(const char *text, size_t length,
                          struct weavecRtSymbol *symbol) {
  const char *colon[2] = {NULL, NULL};
  const char *at;
  unsigned found = 0;
  for (at = text + length; at > text && found < 2; --at)
    if (at[-1] == ':')
      colon[found++] = at - 1;
  /* (Darwin's atos has no column.) */
  if (found == 0)
    return;
  if (found == 2 && colon[1] != NULL) {
    const unsigned line = (unsigned)strtoul(colon[1] + 1, NULL, 10);
    const unsigned column = (unsigned)strtoul(colon[0] + 1, NULL, 10);
    if (line != 0 && strncmp(text, "??", 2) != 0) {
      symbol->file = shortPath(text, (size_t)(colon[1] - text));
      symbol->line = line;
      symbol->column = column;
      return;
    }
  }
  if (strncmp(text, "??", 2) != 0) {
    const unsigned line = (unsigned)strtoul(colon[0] + 1, NULL, 10);
    if (line != 0) {
      symbol->file = shortPath(text, (size_t)(colon[0] - text));
      symbol->line = line;
    }
  }
}

/* One frame's answer from `reader`, up to its blank line; atos' lines are
 * `function (in module) (file:line)`, llvm-symbolizer's pairs of lines. */
static void readAnswer(struct Reader *reader, int atos, uint64_t deadline,
                       struct weavecRtFrameInfo *frame) {
  /* (Static, as every large buffer of a report: a fault's report runs on
   * the program's signal stack, which may be small. Reports hold a lock.) */
  static char line[2048];
  while (readLine(reader, line, sizeof line, deadline) && line[0] != 0) {
    struct weavecRtSymbol symbol = {NULL, NULL, 0, 0};
    if (atos) {
      const char *in = strstr(line, " (in ");
      const size_t length = strlen(line);
      /* (An address it cannot name, and the rest of a trap message that
       * held a newline.) */
      if (in == NULL || in == line || strncmp(line, "0x", 2) == 0)
        continue;
      symbol.function = keep(line, (size_t)(in - line));
      if (line[length - 1] == ')') {
        const char *open = strrchr(line, '(');
        if (open != NULL && open > in + 4)
          parseLocation(open + 1, (size_t)(line + length - 1 - open - 1),
                        &symbol);
      }
    } else {
      static char where[2048];
      if (!readLine(reader, where, sizeof where, deadline))
        break;
      if (strcmp(line, "??") != 0)
        symbol.function = keep(line, strlen(line));
      parseLocation(where, strlen(where), &symbol);
      if (symbol.function == NULL && symbol.file == NULL)
        continue;
    }
    /* (Clang's trap messages are frames of a function of their own.) */
    if (symbol.function != NULL &&
        strncmp(symbol.function, "__clang_trap_msg", 16) == 0)
      continue;
    if (frame->count < sizeof frame->lines / sizeof frame->lines[0])
      frame->lines[frame->count++] = symbol;
  }
}

static void putHexText(char *out, uintptr_t value) {
  char digits[20];
  unsigned count = 0;
  do {
    digits[count++] = "0123456789abcdef"[value & 15];
    value >>= 4;
  } while (value != 0);
  *out++ = '0';
  *out++ = 'x';
  while (count != 0)
    *out++ = digits[--count];
  *out = 0;
}

/* One symbolization's modules, and its frames grouped by module. */
enum { MaxFrames = 3 * WeavecRtReportFrames };
static struct Module modules[MaxFrames];
static unsigned done[MaxFrames];
static unsigned grouped[MaxFrames];

/* Asks the kept weavec-cc (started now if need be) about `indexes`. */
static int askDriver(const uintptr_t *pcs, const unsigned *indexes,
                     unsigned count, struct weavecRtFrameInfo *out,
                     uint64_t deadline) {
  static struct Reader reader;
  unsigned i;
  char *argv[3] = {symbolizer, (char *)"--weavec-symbolize", NULL};
  (void)pcs;
  if (keptFailed || symbolizer[0] == 0)
    return 0;
  /* The parent's, after a fork: its pipes are closed, not inherited by
   * this process's own symbolizer. */
  if (kept.pid != 0 && kept.owner != getpid()) {
    (void)close(kept.in), (void)close(kept.out);
    kept.pid = 0, kept.in = kept.out = -1;
  }
  if (kept.pid == 0) {
    if (!spawn(&kept, argv)) {
      keptFailed = 1;
      return 0;
    }
    reader.fd = kept.out;
    reader.start = reader.end = 0;
  }
  for (i = 0; i < count; ++i) {
    char request[1200];
    char offset[24];
    const struct weavecRtFrameInfo *frame = &out[indexes[i]];
    const size_t path = strlen(frame->module);
    if (path + 40 > sizeof request)
      continue;
    putHexText(offset, modules[indexes[i]].address);
    memcpy(request, "CODE \"", 6);
    memcpy(request + 6, frame->module, path);
    memcpy(request + 6 + path, "\" ", 2);
    memcpy(request + 8 + path, offset, strlen(offset));
    request[8 + path + strlen(offset)] = '\n';
    if (!writeAll(kept.in, request, 9 + path + strlen(offset)))
      break;
    readAnswer(&reader, 0, deadline, &out[indexes[i]]);
    if (now() >= deadline)
      break;
  }
  if (i < count) {
    /* A symbolizer that stopped answering is not asked again. */
    closeChild(&kept, 1);
    keptFailed = 1;
  }
  return i == count;
}

#if defined(__APPLE__)
/* Runs atos once for one module's frames. */
static int askAtos(const uintptr_t *pcs, const unsigned *indexes,
                   unsigned count, struct weavecRtFrameInfo *out,
                   uintptr_t header, uint64_t deadline) {
  struct Child child = {0, -1, -1, 0};
  static struct Reader reader;
  char load[24];
  char *argv[] = {(char *)"/usr/bin/atos", (char *)"-fullPath", (char *)"-o",
                  (char *)out[indexes[0]].module, (char *)"-l", load,
                  (char *)"-i", NULL};
  unsigned i;
  int answered = 0;
  putHexText(load, header);
  if (access(argv[0], X_OK) != 0 || !spawn(&child, argv))
    return 0;
  for (i = 0; i < count; ++i) {
    char address[24];
    putHexText(address, pcs[indexes[i]]);
    address[strlen(address) + 1] = 0;
    address[strlen(address)] = '\n';
    if (!writeAll(child.in, address, strlen(address)))
      break;
  }
  (void)close(child.in);
  child.in = -1;
  reader.fd = child.out;
  reader.start = reader.end = 0;
  for (i = 0; i < count && now() < deadline; ++i) {
    readAnswer(&reader, 1, deadline, &out[indexes[i]]);
    answered += out[indexes[i]].count != 0;
  }
  closeChild(&child, now() >= deadline);
  return answered != 0;
}
#endif

/* Names what the symbolizer did not: the symbol dladdr finds. */
static void fallBack(uintptr_t pc, struct weavecRtFrameInfo *frame) {
  Dl_info info;
  if (frame->count != 0)
    return;
  memset(&frame->lines[0], 0, sizeof frame->lines[0]);
  if (dladdr((const void *)pc, &info) != 0 && info.dli_sname != NULL)
    frame->lines[0].function = keep(info.dli_sname, strlen(info.dli_sname));
  frame->count = 1;
}

void weavecRtSymbolize(const uintptr_t *pcs, unsigned count,
                       struct weavecRtFrameInfo *out) {
  const uint64_t deadline = now() + 5000;
  unsigned i;
  arenaUsed = 0;
  if (count > MaxFrames)
    count = MaxFrames;
  if (getcwd(workDirectory, sizeof workDirectory) == NULL)
    workDirectory[0] = 0;
  for (i = 0; i < count; ++i) {
    out[i].count = 0;
    out[i].offset = 0;
    out[i].module = NULL;
    done[i] = 0;
    if (moduleOf(pcs[i], &modules[i])) {
      out[i].module = modules[i].path;
      out[i].offset = modules[i].offset;
    }
  }
  /* One module's frames at a time. */
  for (i = 0; i < count && !symbolizeOff; ++i) {
    unsigned n = 0;
    unsigned j;
    if (done[i] || out[i].module == NULL)
      continue;
    for (j = i; j < count; ++j)
      if (!done[j] && out[j].module != NULL &&
          strcmp(out[j].module, out[i].module) == 0) {
        grouped[n++] = j;
        done[j] = 1;
      }
#if defined(__APPLE__)
    if (!symbolizerChosen &&
        askAtos(pcs, grouped, n, out, modules[i].header, deadline))
      continue;
#endif
    (void)askDriver(pcs, grouped, n, out, deadline);
  }
  for (i = 0; i < count; ++i)
    fallBack(pcs[i], &out[i]);
}
