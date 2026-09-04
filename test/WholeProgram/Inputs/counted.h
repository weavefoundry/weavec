/* A reference-counted object whose ref/unref live in counted.c (RFC 0010). */
#ifndef WEAVEC_TEST_COUNTED_H
#define WEAVEC_TEST_COUNTED_H

struct counted {
  int rc;
  char *name;
};

struct counted *counted_new(void);
struct counted *counted_ref(struct counted *c);
void counted_unref(struct counted *c);

#endif /* WEAVEC_TEST_COUNTED_H */
