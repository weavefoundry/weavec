/* RFC 0028: independent regression beyond the frozen population. */
#include "../library.h"
#include <stdlib.h>
static void release(void *p) { (void)p; }
int main(void) { hooks_set(malloc,release); struct node *p=hook_node(); hook_destroy(p); }
