/* Declarations for handle.c (RFC 0007 whole-program tests). */
#ifndef WEAVEC_TEST_HANDLE_H
#define WEAVEC_TEST_HANDLE_H

#include <stdio.h>

FILE *log_open(const char *path);
void log_close(FILE *f);
void xfree(void *p);
int log_peek(FILE *f);

#endif /* WEAVEC_TEST_HANDLE_H */
