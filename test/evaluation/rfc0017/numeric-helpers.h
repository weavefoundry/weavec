// RFC 0017: shared numeric interfaces for the added regression population.
#ifndef WEAVEC_EVALUATION_RFC0017_NUMERIC_HELPERS_H
#define WEAVEC_EVALUATION_RFC0017_NUMERIC_HELPERS_H
#include "../../Inputs/prelude.h"
unsigned char narrow_count(unsigned n);
void narrow_count_out(unsigned n, size_t *out);
void fill_min(char *p, size_t n, size_t cap);
int checked_size(size_t n, size_t m, size_t *out);
#endif
