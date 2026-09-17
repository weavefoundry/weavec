#include <stddef.h>
struct writer { unsigned flags; unsigned char *data; size_t used, capacity; };
int emit(const unsigned char *input,size_t n,struct writer *w) {
    if(!n)return 1;
    if(w->used>=w->capacity)return 0;
    w->data[w->used]=input[0];w->used++;
    if(!emit(input,n,w))return 0;
    return 1;
}
int main(void) {unsigned char input[]={1,2,3},output[3];struct writer w={7,output,0,3};(void)emit(input,3,&w);return 0;}
