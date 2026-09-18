#include "api.h"
int emit(const unsigned char *input,size_t n,struct writer *w) {
    if(!n)return 1;
    if(w->used>=w->capacity)return 0;
    w->data[w->used]=input[0];w->used++;
    if(!emit(input+1,n-1,w))return 0;
    return 1;
}
