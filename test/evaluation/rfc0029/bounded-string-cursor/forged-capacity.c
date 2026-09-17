#include "writer.h"
#include <stdlib.h>
#include <string.h>
int main(void) {
  unsigned char *data = malloc(4);
  if (!data) return 0;
  memcpy(data, "abc", 4);
  struct writer w = {data, 8, 4, 0};
  finish(&w);
  if (w.length >= w.capacity) data[4] = 1;
  else data[w.length] = 0;
  free(data);
  return 0;
}
