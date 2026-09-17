#include "reader.h"
int take(struct reader *r) {
  if (r->pos == r->limit) return -1;
  ((unsigned char*)r->data)[r->pos]=0;
  int value=r->data[r->pos];
  ++r->pos;
  return value;
}
