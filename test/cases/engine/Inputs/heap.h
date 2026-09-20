#ifndef WEAVEC_EVALUATION_HEAP_H
#define WEAVEC_EVALUATION_HEAP_H
#include "prelude.h"
struct box { char *data; };
struct box *box_new(void);
struct box *box_wrap(char *);
#endif
