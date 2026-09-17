#include "api.h"
#include <stdlib.h>
static void *short_one(size_t count) { (void)count; return malloc(1); }
int main(void) { void *p = make(short_one, 19); free(p); return 0; }
