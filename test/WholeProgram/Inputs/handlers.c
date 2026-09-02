/* A unit that registers a callback (takes a function's address) so the type
 * has a candidate in the program (RFC 0005, indirect calls). */
#include "../../Inputs/prelude.h"

static void on_done(void *p) { free(p); }

void (*get_handler(void))(void *) { return on_done; }
