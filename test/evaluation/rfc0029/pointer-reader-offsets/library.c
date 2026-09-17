#include "reader.h"
int scan(struct reader *r) {
 if(!r || !r->data || r->position>=r->capacity)return 0;
 const unsigned char *cursor=r->data+r->position;
 while((size_t)(cursor-r->data)<r->capacity) {
  if(*cursor=='x')return 1;
  ++cursor;
 }
 return 0;
}
