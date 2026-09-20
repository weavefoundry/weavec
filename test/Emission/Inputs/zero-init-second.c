/* A second unit of rfc0030-zero-init-target.c: the warning is not repeated. */
void *malloc(__typeof__(sizeof 0));
void *again(void) { return malloc(8); }
