// Signal handler frees a global the main flow still uses.
// ASAN
#include <signal.h>
#include <stdlib.h>
static char *g_buf;
static void handler(int sig) { (void)sig; free(g_buf); }
int main(void) {
  g_buf = malloc(8);
  if (!g_buf) return 1;
  g_buf[0] = 1;
  signal(SIGUSR1, handler);
  raise(SIGUSR1);
  return g_buf[0]; // BUG: use-after-free // NOT-PROVEN: temporal
}
