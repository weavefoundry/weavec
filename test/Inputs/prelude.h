/* Minimal libc surface so tests do not depend on system headers. */
#ifndef WEAVEC_TEST_PRELUDE_H
#define WEAVEC_TEST_PRELUDE_H

typedef unsigned long size_t;

void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
/* The opaque "look at this pointer" helper. It is annotated because a call
 * to an unannotated external function warns by default (RFC 0003). */
void use(const void *__attribute__((annotate("weavec.borrowed"))) ptr);

#define NULL ((void *)0)

#endif /* WEAVEC_TEST_PRELUDE_H */
