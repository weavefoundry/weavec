/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  struct buffer *p = buffer_new(8); if (!p) return 0; if (buffer_append(p, 42)) (void)buffer_data(p)[0]; buffer_destroy(p);
  return 0;
}
