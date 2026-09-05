/* Helpers for rfc0011-extents.c (RFC 0011). */
#include "../../Inputs/prelude.h"
#include "buffers.h"

#define offsetof(T, m) __builtin_offsetof(T, m)
#define container_of(p, T, m) ((T *)((char *)(p) - offsetof(T, m)))

char *buffer_new(size_t n) { return malloc(n); }

void buffer_put8(char *b) {
  for (int i = 0; i < 8; i++)
    b[i] = (char)i;
}

void buffer_fill(char *b, size_t n) {
  for (size_t i = 0; i < n; i++)
    b[i] = 0;
}

void wrapped_release(char *payload) {
  struct wrapped *w = container_of(payload, struct wrapped, payload);
  free(w);
}
