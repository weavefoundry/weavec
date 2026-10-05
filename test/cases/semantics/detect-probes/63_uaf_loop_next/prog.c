// RFC 0034 detection probe, case 63 (use after free): a probe written after the 61 blind programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 4 -2 7 -1 3
#include <stdio.h>
#include <stdlib.h>

struct node {
  int val;
  struct node *next;
};

static struct node *build(int argc, char **argv) {
  struct node *head = NULL, **tail = &head;
  for (int i = 1; i < argc; i++) {
    struct node *n = calloc(1, sizeof *n);
    if (!n)
      exit(1);
    n->val = atoi(argv[i]);
    *tail = n;
    tail = &n->next;
  }
  return head;
}

/* drops every negative value */
static struct node *drop_negative(struct node *head) {
  struct node **link = &head;
#ifdef FIX
  struct node *n = head;
  while (n) {
    struct node *next = n->next;
    if (n->val < 0) {
      *link = next;
      free(n);
    } else {
      link = &n->next;
    }
    n = next;
  }
#else
  for (struct node *n = head; n; n = n->next) { // STOP
    if (n->val < 0) {
      *link = n->next;
      free(n);
    } else {
      link = &n->next;
    }
  }
#endif
  return head;
}

int main(int argc, char **argv) {
  struct node *h = drop_negative(build(argc, argv));
  int sum = 0;
  while (h) {
    struct node *n = h->next;
    sum += h->val;
    free(h);
    h = n;
  }
  printf("sum=%d\n", sum);
  return 0;
}
