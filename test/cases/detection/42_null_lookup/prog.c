// RFC 0034 detection set, case 42 (null dereference): one of the 61 blind bug programs
// of the RFC 0034 milestone (build/eval-2026-10-04/detect). The default build must stop
// the bug at or before its STOP line (a WeaveC error, or a trap whose report-mode check
// is on that line); -DFIX selects the fixed twin, which must build and run clean.
// DETECT: -DFIX
// FLAGS: -O2
// RUN-INPUT: zz a=1 b=2
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct kv {
  char key[16];
  int val;
  struct kv *next;
};

static struct kv *kv_put(struct kv *head, const char *spec) {
  const char *eq = strchr(spec, '=');
  if (!eq)
    return head;
  struct kv *e = calloc(1, sizeof *e);
  if (!e)
    return head;
  size_t klen = (size_t)(eq - spec);
  if (klen >= sizeof e->key)
    klen = sizeof e->key - 1;
  memcpy(e->key, spec, klen);
  e->val = atoi(eq + 1);
  e->next = head;
  return e;
}

static struct kv *kv_find(struct kv *head, const char *key) {
  for (struct kv *e = head; e; e = e->next)
    if (strcmp(e->key, key) == 0)
      return e;
  return NULL;
}

int main(int argc, char **argv) {
  if (argc < 2)
    return 2;
  struct kv *head = NULL;
  for (int i = 2; i < argc; i++)
    head = kv_put(head, argv[i]);
  struct kv *e = kv_find(head, argv[1]);
#ifdef FIX
  if (!e) {
    printf("%s not set\n", argv[1]);
    return 0;
  }
#endif
  printf("%s = %d\n", argv[1], e->val); // STOP
  return 0;
}
