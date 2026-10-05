// RFC 0034 detection set, case 40 (uninitialised pointer): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: apple pear plum fig kiwi
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct item {
  char label[12];
  int weight;
};

struct bag {
  struct item **slots;
  size_t len, cap;
};

static int bag_add(struct bag *b, const char *label, int w) {
  if (b->len == b->cap) {
    size_t ncap = b->cap ? b->cap * 2 : 4;
    struct item **ns = realloc(b->slots, ncap * sizeof *ns);
    if (!ns)
      return -1;
    b->slots = ns;
    b->cap = ncap;
  }
  struct item *it = malloc(sizeof *it);
  if (!it)
    return -1;
  snprintf(it->label, sizeof it->label, "%s", label);
  it->weight = w;
  b->slots[b->len++] = it;
  return 0;
}

static int bag_weight(const struct bag *b) {
  int total = 0;
#ifdef FIX
  for (size_t i = 0; i < b->len; i++)
#else
  for (size_t i = 0; i < b->cap; i++) /* walks the unused slots too */
#endif
    total += b->slots[i]->weight; // STOP
  return total;
}

int main(int argc, char **argv) {
  struct bag b = {0};
  for (int i = 1; i < argc; i++)
    if (bag_add(&b, argv[i], (int)strlen(argv[i])))
      return 1;
  printf("%zu items weigh %d\n", b.len, bag_weight(&b));
  for (size_t i = 0; i < b.len; i++)
    free(b.slots[i]);
  free(b.slots);
  return 0;
}
