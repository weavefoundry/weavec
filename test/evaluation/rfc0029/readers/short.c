#include "cursor.h"
int main(void) {
  const unsigned char data[] = {1, 2};
  struct cursor c = {0, 0, data, 4, 0};
  return (int)consume(&c);
}
