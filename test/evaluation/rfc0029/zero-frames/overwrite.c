#include <stddef.h>
#include <string.h>
struct writer { unsigned char *data; size_t length, capacity; };
static int emit(struct writer *w) {
 if(w->length>w->capacity || w->capacity-w->length<3) return 0;
 unsigned char *p=w->data+w->length;
 p[0]='x';p[1]='y';p[2]=0;w->length+=2;p[2]='z';return 1;
}
int main(void) { unsigned char bytes[3]; struct writer w={bytes,0,3};
 if(!emit(&w))return 0; return strlen((char*)w.data)!=2;
}
