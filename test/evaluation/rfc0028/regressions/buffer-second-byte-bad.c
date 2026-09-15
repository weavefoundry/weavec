/* RFC 0028: independent regression beyond the frozen population. */
#include "../library.h"
int main(void) { struct buffer *p=buffer_new(8); if (!p) return 0; if (buffer_append(p,42)) (void)buffer_data(p)[1]; buffer_destroy(p); }
