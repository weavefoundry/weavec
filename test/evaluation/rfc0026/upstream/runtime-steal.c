/* RFC 0026: unchanged Jansson sources with runtime growth. */
#include "strbuffer.h"
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    (void)argv; unsigned n=(unsigned)argc; if(n>1000000)return 0;
    strbuffer_t b; if(strbuffer_init(&b))return 0;
    for(unsigned i=0;i<n;++i)
        if(strbuffer_append_byte(&b,'x')) {strbuffer_close(&b);return 0;}
    size_t length=b.length; char *owned=strbuffer_steal_value(&b);
    int ok=length ? owned[length-1]=='x' : owned[0]==0;
    free(owned);strbuffer_close(&b);return !ok;
}
