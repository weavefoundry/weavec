// CLEAN
// RFC 0030 §9.3 resolves the registry's slot to its one target, so the loop's
// second turn sees the first turn's release of an element it cannot tell apart:
// a free a later iteration may repeat is a possible finding, so a warning (§3.1,
// as in repros/arrloop.c). Before S7 the call was unresolved and the release was
// of unknown origin, which is never diagnosed.
// ALLOW: double-free
// ASAN
#include <stdlib.h>
typedef void (*hook_fn)(void *);
static hook_fn g_hooks[4];
static void *g_args[4];
static int g_n;
static void on_exit_hook(hook_fn fn, void *arg) { if (g_n < 4) { g_hooks[g_n] = fn; g_args[g_n] = arg; g_n++; } }
static void run_hooks(void) { for (int i = 0; i < g_n; i++) g_hooks[i](g_args[i]); }
static void drop(void *p) { free(p); }
int main(void) {
  char *p = malloc(8);
  if (!p) return 1;
  p[0] = 1;
  int r = p[0];
  on_exit_hook(drop, p);
  run_hooks();
  return r;
}
