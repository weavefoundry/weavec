// RFC 0034 detection set, case 54 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: alpha beta reset gamma
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cache {
  int hits;
  char last[32];
};

static struct cache *g_cache;

static void cache_reset(void) {
  free(g_cache);
#ifdef FIX
  g_cache = NULL;
#endif
}

static void cache_touch(const char *key) {
  if (!g_cache) {
    g_cache = calloc(1, sizeof *g_cache);
    if (!g_cache)
      exit(1);
  }
  g_cache->hits++; // STOP
  snprintf(g_cache->last, sizeof g_cache->last, "%s", key);
}

int main(int argc, char **argv) {
  for (int i = 1; i < argc; i++) {
    if (strcmp(argv[i], "reset") == 0)
      cache_reset();
    else
      cache_touch(argv[i]);
  }
  if (g_cache)
    printf("hits=%d last=%s\n", g_cache->hits, g_cache->last);
  return 0;
}
