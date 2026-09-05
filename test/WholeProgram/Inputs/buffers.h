/* Sized buffers whose helpers live in buffers.c (RFC 0011). */
#ifndef WEAVEC_TEST_BUFFERS_H
#define WEAVEC_TEST_BUFFERS_H

typedef unsigned long size_t;

struct wrapped {
  int tag;
  char payload[8];
};

/* `n` bytes, or NULL. */
char *buffer_new(size_t n);
/* Writes `b[0..8)`. */
void buffer_put8(char *b);
/* Writes `b[0..n)`. */
void buffer_fill(char *b, size_t n);
/* Releases the `struct wrapped` that `payload` sits inside. */
void wrapped_release(char *payload);

#endif /* WEAVEC_TEST_BUFFERS_H */
