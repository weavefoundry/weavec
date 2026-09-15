/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  struct node *p = nodes(2), *q = p; destroy(p); (void)count_nodes(q);
  return 0;
}
