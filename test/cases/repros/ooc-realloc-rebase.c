// Held-out repro (RFC 0031 Motivation, §11.1): libyaml src/api.c:74-154 (yaml_string_extend,
// yaml_stack_extend, yaml_queue_extend) grow a buffer described by start/pointer/end
// out-parameters with 'new_start = yaml_realloc(*start, ...)' and then rebase the other
// pointers by arithmetic on the stale value: '*top = new_start + (*top - *start)'. Only the
// difference of the old pointers is used, never their targets.
// v0.11.0 reports definite 'use-after-move' errors on each '*start', '*top', '*head' and
// '*end' in that arithmetic (api.c:81, 83, 84, 130, 131, 152-154), which stop the build.
// Reduced from libyaml 90a56d45; yaml_realloc is its own wrapper, kept as written.
// intended: no finding; subtracting two pointers into the same moved-from object reads
// neither object (RFC 0031 §5.2).
// CLEAN
// ASAN
#include <limits.h>
#include <stdlib.h>
#include <string.h>
typedef unsigned char yaml_char_t;
static void *yaml_realloc(void *ptr, size_t size) {
  return ptr ? realloc(ptr, size ? size : 1) : malloc(size ? size : 1);
}
int yaml_string_extend(yaml_char_t **start, yaml_char_t **pointer, yaml_char_t **end) {
  yaml_char_t *new_start = (yaml_char_t *)yaml_realloc((void *)*start, (size_t)(*end - *start) * 2);
  if (!new_start) return 0;
  memset(new_start + (*end - *start), 0, (size_t)(*end - *start));
  *pointer = new_start + (*pointer - *start);
  *end = new_start + (*end - *start) * 2;
  *start = new_start;
  return 1;
}
int yaml_stack_extend(void **start, void **top, void **end) {
  void *new_start;
  if ((char *)*end - (char *)*start >= INT_MAX / 2) return 0;
  new_start = yaml_realloc(*start, (size_t)((char *)*end - (char *)*start) * 2);
  if (!new_start) return 0;
  *top = (char *)new_start + ((char *)*top - (char *)*start);
  *end = (char *)new_start + ((char *)*end - (char *)*start) * 2;
  *start = new_start;
  return 1;
}
int yaml_queue_extend(void **start, void **head, void **tail, void **end) {
  if (*start == *head && *tail == *end) {
    void *new_start = yaml_realloc(*start, (size_t)((char *)*end - (char *)*start) * 2);
    if (!new_start) return 0;
    *head = (char *)new_start + ((char *)*head - (char *)*start);
    *tail = (char *)new_start + ((char *)*tail - (char *)*start);
    *end = (char *)new_start + ((char *)*end - (char *)*start) * 2;
    *start = new_start;
  }
  if (*tail == *end) {
    if (*head != *tail) memmove(*start, *head, (size_t)((char *)*tail - (char *)*head));
    *tail = (char *)*tail - (char *)*head + (char *)*start;
    *head = *start;
  }
  return 1;
}
int main(void) {
  struct { yaml_char_t *start, *pointer, *end; } s;
  s.start = malloc(16);
  if (!s.start) return 1;
  memset(s.start, 0, 16);
  s.pointer = s.start + 10;
  s.end = s.start + 16;
  if (!yaml_string_extend(&s.start, &s.pointer, &s.end)) { free(s.start); return 1; }
  *s.pointer = 'x';
  struct { int *start, *top, *end; } st;
  st.start = malloc(4 * sizeof(int));
  if (!st.start) { free(s.start); return 1; }
  st.top = st.start + 4;
  st.end = st.start + 4;
  if (!yaml_stack_extend((void **)&st.start, (void **)&st.top, (void **)&st.end)) {
    free(s.start); free(st.start); return 1;
  }
  *st.top++ = 5;
  struct { int *start, *head, *tail, *end; } q;
  q.start = malloc(4 * sizeof(int));
  if (!q.start) { free(s.start); free(st.start); return 1; }
  q.head = q.start;
  q.tail = q.start + 4;
  q.end = q.start + 4;
  if (!yaml_queue_extend((void **)&q.start, (void **)&q.head, (void **)&q.tail, (void **)&q.end)) {
    free(s.start); free(st.start); free(q.start); return 1;
  }
  *q.tail++ = 6;
  int r = (st.top[-1] == 5 && q.tail[-1] == 6 && s.start[10] == 'x') ? 0 : 1;
  free(s.start);
  free(st.start);
  free(q.start);
  return r;
}
