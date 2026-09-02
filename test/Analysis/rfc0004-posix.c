// RFC 0004, "The library table": the shipped table covers the POSIX and
// common GNU/BSD functions real programs call, so checked code that uses them
// neither warns (default) nor errors (--strict-externs), and their ownership
// effects are modelled.
// RUN: not %weavec %s -- 2>&1 | FileCheck %s
// RUN: not %weavec --strict-externs %s -- 2>&1 | FileCheck %s
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// `getline` stores a fresh buffer through its first argument.
void lines(FILE *f) {
  char *line = NULL;
  size_t cap = 0;
  while (getline(&line, &cap, f) != -1)
    fputs(line, stdout);
  free(line);
  // CHECK: rfc0004-posix.c:[[@LINE+1]]:8: error: use of 'line' after it was freed [weavec::use-after-free]
  puts(line);
}

// `asprintf` too.
void formatted(int n) {
  char *s;
  if (asprintf(&s, "%d", n) < 0)
    return;
  free(s);
  // CHECK: rfc0004-posix.c:[[@LINE+1]]:3: error: 's' is freed twice [weavec::double-free]
  free(s);
}

// Directory streams are owned; an entry lives inside the stream.
void listing(const char *path) {
  DIR *d = opendir(path);
  if (!d)
    return;
  struct dirent *e = readdir(d);
  closedir(d);
  // CHECK: rfc0004-posix.c:[[@LINE+1]]:8: error: use of 'e' after it was freed [weavec::use-after-free]
  puts(e->d_name);
  // CHECK: rfc0004-posix.c:[[@LINE-3]]:3: note: freed here (through 'd')
}

// Mappings, address lists and dynamic library handles are owned.
void owned_handles(const char *host) {
  void *m = mmap(NULL, 4096, PROT_READ, MAP_PRIVATE, -1, 0);
  if (m != MAP_FAILED)
    munmap(m, 4096);
  struct addrinfo *res = NULL;
  if (getaddrinfo(host, "80", NULL, &res) == 0) {
    freeaddrinfo(res);
    // CHECK: rfc0004-posix.c:[[@LINE+1]]:5: error: 'res' is freed twice [weavec::double-free]
    freeaddrinfo(res);
  }
  void *lib = dlopen("libm.so", RTLD_NOW);
  if (lib)
    dlclose(lib);
}

// Everyday POSIX with no ownership effects: nothing to report, nothing to
// warn about, even in strict mode.
static void *worker(void *arg) { return arg; }
int everyday(const char *path, char *buf, size_t n) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return errno;
  struct stat st;
  if (fstat(fd, &st) == 0 && read(fd, buf, n) > 0)
    write(1, buf, n);
  close(fd);
  char cwd[64];
  if (getcwd(cwd, sizeof cwd))
    puts(cwd);
  char *save;
  for (char *tok = strtok_r(buf, " ", &save); tok; tok = strtok_r(NULL, " ", &save))
    puts(tok);
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  struct tm tm;
  time_t now = ts.tv_sec;
  localtime_r(&now, &tm);
  pthread_mutex_t mu;
  pthread_mutex_init(&mu, NULL);
  pthread_mutex_lock(&mu);
  pthread_mutex_unlock(&mu);
  pthread_mutex_destroy(&mu);
  pthread_t th;
  if (pthread_create(&th, NULL, worker, buf) == 0)
    pthread_join(th, NULL);
  int s = socket(AF_INET, SOCK_STREAM, 0);
  if (s >= 0) {
    struct sockaddr_storage addr;
    socklen_t len = sizeof addr;
    getsockname(s, (struct sockaddr *)&addr, &len);
    close(s);
  }
  return 0;
}

// CHECK: 4 errors generated.
