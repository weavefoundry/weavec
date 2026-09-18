#include <stddef.h>
#include <string.h>
struct output { char *data; size_t capacity, cursor, depth; };
int append(struct output *p, char c) {
 if(p->cursor>=p->capacity)return 0;
 p->data[p->cursor++]=c;return 1;
}
void update(struct output *p) {
 if(!p || !p->data)return;
 const char *old=p->data+p->cursor;
 p->cursor+=strlen(old);
}
int main(void) {char data[]="abc";struct output p={data,sizeof data,0,0};update(&p);return 0;}
