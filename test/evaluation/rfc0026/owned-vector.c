#define ELEMENT unsigned char *
#include "buffer.h"
static void destroy_elements(struct buffer *b) {
    for(size_t i=0;i<b->length;++i)free(b->data[i]);
    destroy(b);
}
int main(int argc,char **argv) {
    (void)argv; unsigned n=(unsigned)argc;if(n>1000000)return 0;
    struct buffer b={0};
    for(unsigned i=0;i<n;++i){
        unsigned char *p=malloc(1);if(!p){destroy_elements(&b);return 0;}*p=7;
        if(append(&b,p)){free(p);destroy_elements(&b);return 0;}
    }
    destroy_elements(&b);return 0;
}
