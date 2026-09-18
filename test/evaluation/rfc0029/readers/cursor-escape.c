#include "cursor.h"
int main(void) {
  const unsigned char data[] = {1, 2};
  struct cursor c = {0, 0, data, sizeof data, 3};
  unsigned char out = 0;
  return take(&c, &out);
}
