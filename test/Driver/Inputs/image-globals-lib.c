// A shared image's global table and an accessor that indexes it through a
// pointer (image-globals.c).
int lib_table[4] = {1, 2, 3, 4};
int *volatile lib_view = lib_table;

int lib_read(long i) { return lib_view[i]; }
