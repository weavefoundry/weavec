/* RFC 0026: unchanged Jansson sources with runtime growth. */
#include "strbuffer.h"
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    (void)argv; unsigned n=(unsigned)argc; if(n>1000000)return 0;
    strbuffer_t b; if(strbuffer_init(&b))return 0;
    for(unsigned i=0;i<n;++i)
        if(strbuffer_append_byte(&b,'x')) {strbuffer_close(&b);return 0;}
    int result=0;
    if(b.size-b.length>1)result=b.value[b.size-1];
    strbuffer_close(&b);return result;
}
