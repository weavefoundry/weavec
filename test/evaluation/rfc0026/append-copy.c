#include "buffer.h"
static int append_copy(struct buffer *b, const unsigned char *src, size_t n) {
    if(n > SIZE_MAX - b->length) return -1;
    size_t length=b->length+n;
    if(reserve(b,length))return -1;
    if(n) memcpy(b->data+b->length,src,n);
    b->length=length;
    return 0;
}
int main(void) { struct buffer b={0}; unsigned char bytes[32]={1};
    if(append_copy(&b,bytes,32)){destroy(&b);return 0;}
    int result=b.data[31]; destroy(&b);return result; }
