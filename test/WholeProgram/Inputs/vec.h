/* A growable array whose pointer field is counted by a sibling, without an
   annotation: the pair is inferred from vec.c (RFC 0012). */
#ifndef WEAVEC_TEST_VEC_H
#define WEAVEC_TEST_VEC_H

typedef unsigned long size_t;

struct vec {
  int *items;
  size_t n;
  size_t cap;
};

/* Allocates `cap` items. */
int vec_init(struct vec *v, size_t cap);
/* Appends; grows when full. */
int vec_push(struct vec *v, int x);
void vec_free(struct vec *v);

/* A pair every store refutes: `raw` is sometimes a caller's buffer. */
struct view {
  int *raw;
  size_t len;
};

void view_own(struct view *w, size_t len);
void view_borrow(struct view *w, int *p, size_t len);

#endif /* WEAVEC_TEST_VEC_H */
