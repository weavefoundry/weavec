/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  struct node *p = nodes(2); destroy(p); destroy(p);
  return 0;
}
