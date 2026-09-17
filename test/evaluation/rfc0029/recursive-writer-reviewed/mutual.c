#include <stddef.h>
struct writer { unsigned flags; unsigned char *data; size_t used, capacity; };
int other(const unsigned char *,size_t,struct writer*);
int emit(const unsigned char *input,size_t n,struct writer *w) {
    if(!n)return 1;
    if(w->used>=w->capacity)return 0;
    w->data[w->used]=input[0];w->used++;
    if(!other(input+1,n-1,w))return 0;
    return 1;
}
int other(const unsigned char *input,size_t n,struct writer*w){return emit(input,n,w);}
int main(void){unsigned char input[]={1,2,3},output[3];struct writer w={7,output,0,3};(void)emit(input,3,&w);if(w.used)return w.data[w.used-1];return 0;}
