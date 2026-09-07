// RFC 0015: shared declarations exercise separate summary interfaces.
#ifndef WEAVEC_TEST_ARRAY15_H
#define WEAVEC_TEST_ARRAY15_H
#include "../../Inputs/prelude.h"
void array15_drop(char **a, int i);
void array15_copy(char **d, char **s, size_t n);
void array15_compact(char **a);
char **array15_clone(char **source, size_t n);
void array15_clear(char **a, size_t n);
void array15_fill(char **a, int n);
#endif
