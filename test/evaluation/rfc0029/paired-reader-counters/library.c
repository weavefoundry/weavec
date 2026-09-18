#include "reader.h"
#include <stdlib.h>
#include <string.h>
unsigned char *copy_digits(struct reader *r) {
    size_t i=0, count=0;
    if (!r || !r->data) return 0;
    for (i=0; r->position+i<r->capacity; ++i) {
        switch ((r->data+r->position)[i]) {
        case '0': case '1': ++count; break;
        default: goto done;
        }
    }
done: ;
    unsigned char *out=malloc(count+1);
    if (!out) return 0;
    memcpy(out,r->data+r->position,count);
    out[count]=0;
    return out;
}
