// RFC 0034 detection set, case 23 (use after free): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: 1 2 3 4
#include <stdio.h>
#include <stdlib.h>

struct node {
  int val;
  struct node *next;
};

struct list {
  struct node *head;
  size_t len;
};

static int list_push(struct list *l, int v) {
  struct node *n = malloc(sizeof *n);
  if (!n)
    return -1;
  n->val = v;
  n->next = l->head;
  l->head = n;
  l->len++;
  return 0;
}

static void list_pop(struct list *l) {
  struct node *n = l->head;
  if (!n)
    return;
  l->head = n->next;
  l->len--;
  free(n);
}

int main(int argc, char **argv) {
  struct list l = {0};
  for (int i = 1; i < argc; i++)
    if (list_push(&l, atoi(argv[i])))
      return 1;
  while (l.len > 1) {
    struct node *top = l.head;
#ifdef FIX
    int v = top->val;
    list_pop(&l);
    printf("popped %d\n", v);
#else
    list_pop(&l);
    printf("popped %d\n", top->val); // STOP
#endif
  }
  list_pop(&l);
  return 0;
}
