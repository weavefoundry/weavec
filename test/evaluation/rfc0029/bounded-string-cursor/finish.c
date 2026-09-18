#include "writer.h"
#include <string.h>
void finish(struct writer *w) {
  if (!w || !w->data) return;
  const unsigned char *next = w->data + w->length;
  w->length += strlen((const char *)next);
}
