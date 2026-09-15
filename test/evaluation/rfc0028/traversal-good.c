/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  struct node *p = nodes(3); (void)count_nodes(p); destroy(p);
  return 0;
}
