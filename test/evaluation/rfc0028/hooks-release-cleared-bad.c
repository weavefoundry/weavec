/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  hooks_reset(); struct node *p = hook_node(); hook_forget(); hook_destroy(p);
  return 0;
}
