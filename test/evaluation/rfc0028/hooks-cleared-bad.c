/* RFC 0028: frozen public-header client. */
#include "library.h"
#include <stdlib.h>
int main(int argc, char **argv) {
  (void)argc; (void)argv;
  hooks_reset(); hook_forget(); struct node *p = hook_node(); hook_destroy(p);
  return 0;
}
