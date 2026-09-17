#include <stddef.h>
struct writer { unsigned flags; unsigned char *data; size_t used, capacity; };
int emit(const unsigned char *input,size_t n,struct writer *w) {
    if(!n)return 1;
    if(w->used>=w->capacity){w->used++;return 1;}
    w->data[w->used]=input[0];w->used++;
    if(!emit(input+1,n-1,w))return 0;
    return 1;
}
int main(void){unsigned char input[]={1,2,3},output[2];struct writer w={7,output,0,2};(void)emit(input,3,&w);if(w.used)return w.data[w.used-1];return 0;}
