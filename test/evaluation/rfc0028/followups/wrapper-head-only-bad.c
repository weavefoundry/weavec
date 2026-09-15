/* RFC 0028: freeing one allocation does not consume its children. */
#include "../library.h"
#include <stdlib.h>
static void release(void *p) { free(p); }
int main(void) { hooks_set(malloc, release); struct node *p=nodes(2); hook_destroy(p); }
