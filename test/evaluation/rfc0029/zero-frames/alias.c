#include <stddef.h>
#include <string.h>
struct writer { unsigned char *data; size_t length, capacity; };
static void change(unsigned char *p, unsigned char *q) {
 p[0]=0; q[0]=1;
}
int main(void) { unsigned char x[1];change(x,x);return strlen((char*)x)!=0; }
