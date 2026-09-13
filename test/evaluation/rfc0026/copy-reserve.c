#include "buffer.h"
static int copy_reserve(struct buffer *b, size_t n) {
    if(n <= b->capacity)return 0;
    unsigned char *p=malloc(n); if(!p)return -1;
    if(b->length)memcpy(p,b->data,b->length);
    free(b->data); b->data=p;b->capacity=n;return 0;
}
int main(void) {struct buffer b={0};if(append(&b,7))return 0;
    if(copy_reserve(&b,64)){destroy(&b);return 0;}
    int result=b.data[0];destroy(&b);return result;}
