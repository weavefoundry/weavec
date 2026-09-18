#include "cursor.h"
int main(void) {
  unsigned char data[4]; data[0] = 1;
  struct cursor c = {0, 0, data, sizeof data, 0};
  return (int)consume(&c);
}
