// RFC 0034 detection set, case 52 (type confusion): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: -d red green blue
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void (*visit_fn)(void *ctx, const char *item);

struct counter {
  int count;
};

struct summary {
  int count;
  char longest[24];
  long total;
};

static void count_only(void *ctx, const char *item) {
  (void)item;
  ((struct counter *)ctx)->count++;
}

static void summarize(void *ctx, const char *item) {
  struct summary *s = ctx;
  s->count++;
  s->total += (long)strlen(item); // STOP
  if (strlen(item) > strlen(s->longest))
    snprintf(s->longest, sizeof s->longest, "%s", item);
}

static void walk(char **items, int n, visit_fn fn, void *ctx) {
  for (int i = 0; i < n; i++)
    fn(ctx, items[i]);
}

int main(int argc, char **argv) {
  int detailed = argc > 1 && strcmp(argv[1], "-d") == 0;
#ifdef FIX
  void *ctx = detailed ? calloc(1, sizeof(struct summary)) : calloc(1, sizeof(struct counter));
#else
  void *ctx = calloc(1, sizeof(struct counter)); /* sized for the simple visitor */
#endif
  if (!ctx)
    return 1;
  walk(argv + 1 + detailed, argc - 1 - detailed, detailed ? summarize : count_only, ctx);
  if (detailed) {
    struct summary *s = ctx;
    printf("%d items, %ld chars, longest %s\n", s->count, s->total, s->longest);
  } else {
    printf("%d items\n", ((struct counter *)ctx)->count);
  }
  free(ctx);
  return 0;
}
