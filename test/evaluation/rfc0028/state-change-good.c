/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  configure(0); configure(1); conditional_destroy(nodes(2));
  return 0;
}
