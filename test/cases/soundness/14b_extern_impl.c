// Link-only unit of 14b_unknown_extern_buffer_bug.c: the definition the analysis
// must not see, compiled as plain Clang so the executable links.
// FLAGS: -fno-weavec
static char small[4];
char *get_buffer(void) { return small; }
