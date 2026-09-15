/* RFC 0028: cleanup preserves a separately constructed object. */
#include "../library.h"
#include <stdlib.h>
static void release(void *p) { if (p) free(p); }
int main(void) { hooks_set(malloc, release); struct node *p=hook_node(); struct node *q=hook_node(); hook_destroy(p); hook_destroy(q); }
