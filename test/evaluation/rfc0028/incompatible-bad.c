/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  struct other { unsigned value; void *next; } p = {0, NULL}; destroy((struct node *)&p);
  return 0;
}
