#include "reader.h"
int scan(struct reader *r) {
 if(!r || !r->data || r->position>=r->capacity)return 0;
 const unsigned char *cursor=r->data+r->position;
 while((size_t)(cursor-r->data)<r->capacity) {
  cursor+=8;
  if(*cursor=='x')return 1;
  ++cursor;
 }
 return 0;
}
int main(void) { const unsigned char data[4]={'a','b','c','x'}; struct reader r={data,4,0,0};return scan(&r); }
