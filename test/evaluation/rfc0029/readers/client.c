#include "cursor.h"
int main(void) {
  const unsigned char data[] = {1, 2, 3, 4};
  struct cursor c = {0, 0, data, sizeof data, 0};
  unsigned char first = 0;
  if (!take(&c, &first)) return 1;
  return (int)(consume(&c) + first);
}
