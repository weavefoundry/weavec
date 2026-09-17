#include <stddef.h>
#include <string.h>
struct B {char *data;size_t used,capacity;};
static void copy(struct B *b,char *out) {
 if(b->used>b->capacity)return;
 if (b->data[b->used]) memcpy(out,b->data,b->used+1);
}
int main(void){char in[4]={'a','b','c',0},out[4]={0};struct B b={in,2,2};copy(&b,out);return 0;}
