/* The libc surface the recall cases use, declared here so they do not depend
 * on system headers (RFC 0011, *Recall check*). */
#ifndef WEAVEC_RECALL_H
#define WEAVEC_RECALL_H

typedef unsigned long size_t;

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
char *fgets(char *buf, int n, void *stream);
int printf(const char *fmt, ...);
int puts(const char *s);
int rand(void);

/* Sinks that look at a value without owning it. */
void print_int(int value);
void print_line(const char *__attribute__((annotate("weavec.borrowed"))) s);
void print_bytes(const void *__attribute__((annotate("weavec.borrowed"))) p,
                 size_t n);

#define NULL ((void *)0)

#endif /* WEAVEC_RECALL_H */
