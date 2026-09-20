// Link-only unit of c13_errno_style_extern_global_bug.c, added in the import so the
// executable links: the table the analysis must not see, compiled as plain Clang.
// FLAGS: -fno-weavec
char shared_table[16];
