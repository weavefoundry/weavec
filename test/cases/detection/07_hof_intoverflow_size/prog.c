// RFC 0034 detection set, case 07 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 268435457 < stdin.txt
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

struct item {
  uint32_t id;
  uint32_t qty;
  uint64_t price;
};

#define MAX_ITEMS 1024u

static struct item *items_alloc(uint32_t n) {
#ifdef FIX
  if (n > MAX_ITEMS)
    n = MAX_ITEMS;
  return calloc(n ? n : 1, sizeof(struct item));
#else
  uint32_t bytes = n * (uint32_t)sizeof(struct item); /* wraps for large n */
  return malloc(bytes);
#endif
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  uint32_t n = (uint32_t)strtoul(argv[1], NULL, 10);
  struct item *items = items_alloc(n);
  if (!items)
    return 1;
#ifdef FIX
  if (n > MAX_ITEMS)
    n = MAX_ITEMS;
#endif
  uint32_t k = 0;
  unsigned id, qty;
  unsigned long long price;
  while (k < n && scanf("%u %u %llu", &id, &qty, &price) == 3) {
    items[k].id = id; // STOP
    items[k].qty = qty;
    items[k].price = price;
    k++;
  }
  uint64_t total = 0;
  for (uint32_t i = 0; i < k; i++)
    total += items[i].qty * items[i].price;
  printf("%u items, total %llu\n", k, (unsigned long long)total);
  free(items);
  return 0;
}
