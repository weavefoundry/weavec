/* RFC 0028: independent regression beyond the frozen population. */
#include "../library.h"
void unknown(void);
int main(void) { hooks_reset(); unknown(); struct node *p=hook_node(); hook_destroy(p); }
