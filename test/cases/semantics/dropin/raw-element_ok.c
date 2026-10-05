// RFC 0033 §2: a pointer converted from an integer and stored into one element of an array
// makes nothing else raw; the other elements are ordinary pointers (libuv's kqueue loop keeps
// a count in a spare slot of its watcher table).
// STAGE: S1
// CLEAN
// RUN-INPUT: 3
#include <stdint.h>
#include <stdlib.h>
struct watcher { int fd; };
struct loop { void **watchers; unsigned n; };
static int poll_once(struct loop *l, int nfds, int fd) {
  l->watchers[l->n + 1] = (void *)(uintptr_t)nfds;
  if (fd < 0 || (unsigned)fd >= l->n) return -1;
  struct watcher *w = l->watchers[fd];
  return w ? w->fd : -1;
}
int main(int argc, char **argv) {
  struct loop l;
  struct watcher w[4] = {{10}, {11}, {12}, {13}};
  l.n = 4;
  l.watchers = calloc(l.n + 2, sizeof(void *));
  if (l.watchers == NULL || argc < 2) return 1;
  for (unsigned i = 0; i < l.n; i++) l.watchers[i] = &w[i];
  int r = poll_once(&l, 7, atoi(argv[1]));
  free(l.watchers);
  return r == 13 ? 0 : 1;
}
