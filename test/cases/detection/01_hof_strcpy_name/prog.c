// RFC 0034 detection set, case 01 (heap overflow): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: alice 30 bob 41 carolina 27
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct person {
  char *name;
  int age;
  struct person *next;
};

static struct person *person_new(const char *name, int age) {
  struct person *p = malloc(sizeof *p);
  if (!p)
    return NULL;
#ifdef FIX
  p->name = malloc(strlen(name) + 1);
#else
  p->name = malloc(strlen(name)); /* forgot the terminator */
#endif
  if (!p->name) {
    free(p);
    return NULL;
  }
  strcpy(p->name, name); // STOP
  p->age = age;
  p->next = NULL;
  return p;
}

static void people_free(struct person *p) {
  while (p) {
    struct person *n = p->next;
    free(p->name);
    free(p);
    p = n;
  }
}

int main(int argc, char **argv) {
  struct person *head = NULL;
  for (int i = 1; i + 1 < argc; i += 2) {
    struct person *p = person_new(argv[i], atoi(argv[i + 1]));
    if (!p)
      return 1;
    p->next = head;
    head = p;
  }
  for (struct person *p = head; p; p = p->next)
    printf("%s is %d\n", p->name, p->age);
  people_free(head);
  return 0;
}
